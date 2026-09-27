// Claude CLI — transcript 文件监控（多会话聚合）
// 来源：~/.claude/projects/<project-slug>/<uuid>.jsonl；权限等待补充读取 ~/.claude/sessions/*.json 元数据
// v1.11：多会话聚合——扫描最近活跃的 transcript，各自评估，按 红>黄>绿>蓝>灭 取最高优先级。
'use strict';

const fs = require('fs');
const { BaseCollector } = require('./base');
const { APP_STATE } = require('../state');
const { config_expandHome } = require('./path');
const {
  isProcessMatching,
  recentFiles,
  tailLines,
  nowMs,
} = require('./util');

const DONE_HOLD_MS = 60 * 1000;  // 匹配固件 done 动画(绿闪30s+常亮30s)
const ACTIVE_WINDOW_MS = 15 * 60 * 1000;
const MAX_SESSIONS = 8;

function isPermissionPromptActive(sessionsDir) {
  const files = recentFiles(sessionsDir, {
    ext: '.json',
    maxDepth: 1,
    limit: 128,
  });
  return files.some(({ path: filePath }) => {
    try {
      const session = JSON.parse(fs.readFileSync(filePath, 'utf8'));
      const waitingFor = String(session.waitingFor || '').toLowerCase();
      if (session.status !== 'waiting' || waitingFor !== 'permission prompt') {
        return false;
      }
      const pid = Number(session.pid);
      if (!Number.isInteger(pid) || pid <= 0) return false;
      try {
        process.kill(pid, 0);
        return true;
      } catch {
        return false;
      }
    } catch {
      return false;
    }
  });
}

class ClaudeCollector extends BaseCollector {
  constructor(config) {
    super('claude', 'Claude', config);
  }

  // 评估单个 transcript 会话状态
  evaluateTranscript(filePath, mtimeMs, now) {
    const ageMs = now - mtimeMs;
    const lines = tailLines(filePath);
    // 从尾部向前找最后一条对话行（assistant/user），跳过 system/attachment/file-history-snapshot/
    // atis-latch/last-prompt 等非对话行——Claude 会在 assistant 回复后追加 system 等行，
    // 若直接取原始最后一行会误判为 idle 而错过 DONE(绿)/ERROR(红) 判定。
    let last;
    for (let i = lines.length - 1; i >= 0; i--) {
      const t = lines[i]?.type || lines[i]?.message?.role;
      if (t === 'assistant' || t === 'user') {
        last = lines[i];
        break;
      }
    }
    if (!last) {
      return { state: APP_STATE.IDLE, detail: 'no message' };
    }

    const extract = (row) => {
      const msg = row?.message;
      if (msg) {
        return { role: msg.role, content: msg.content || [], stop: msg.stop_reason };
      }
      return {
        role: typeof row?.type === 'string' ? row.type : undefined,
        content: row?.content || [],
        stop: row?.message?.stop_reason,
      };
    };
    const lastInfo = extract(last);
    // content 防御：Claude transcript 中部分行（如 user/attachment）content 可能是对象而非数组
    const content = Array.isArray(lastInfo.content) ? lastInfo.content : [];

    // 错误：tool_result is_error、content 含 error 块、或 assistant 文本为 API 错误。
    // Claude CLI 会把 API/模型错误写成 assistant text（如 "API Error: 400 {...}"），
    // 若不识别会走 assistant 文本分支误判为 DONE（绿）而非 ERROR（红）。
    const hasError = content.some((c) => {
      if (c?.type === 'tool_result' && c.is_error) return true;
      if (c?.type === 'error') return true;
      if (c?.type === 'text' && /^API Error/i.test((c.text || '').trim())) return true;
      return false;
    });
    if (hasError) {
      return { state: APP_STATE.ERROR, detail: 'tool error' };
    }

    // 完成：assistant 文本结尾（优先于 working；以文件最后写入时刻为基准 30s hold）
    if (lastInfo.role === 'assistant') {
      const hasText = content.some((c) => c?.type === 'text' && c.text && c.text.trim());
      if (hasText) {
        if (now - mtimeMs <= DONE_HOLD_MS) {
          return { state: APP_STATE.DONE, detail: 'assistant reply' };
        }
        return { state: APP_STATE.IDLE, detail: 'done expired' };
      }
    }

    // 工作中：30s 内活跃
    if (ageMs < 30 * 1000) {
      return { state: APP_STATE.WORKING, detail: 'active' };
    }

    // 正在调工具（tool_use 未结束）
    const hasToolUse = content.some((c) => c?.type === 'tool_use');
    const hasToolResult = content.some((c) => c?.type === 'tool_result');
    if (hasToolUse && !hasToolResult) {
      return { state: APP_STATE.WORKING, detail: 'tool call' };
    }

    // 用户消息结尾 → 待命（等待输入）
    if (lastInfo.role === 'user') {
      return { state: APP_STATE.IDLE, detail: 'awaiting input' };
    }

    return { state: APP_STATE.IDLE, detail: 'idle' };
  }

  async collect() {
    const cfg = this.config || {};
    const projectsDir = config_expandHome(cfg.projects_path);
    // 精确进程匹配（config.procInclude/procExclude 排除 shim 命令行里的 claude 模型名等误匹配）
    const running = isProcessMatching({
      include: cfg.procInclude || ['claude'],
      exclude: cfg.procExclude || [],
    });
    // 进程退出 → 尽快灭灯（连续 2 轮 ~4s 确认）
    this._downStreak = running ? 0 : (this._downStreak || 0) + 1;
    if (this._downStreak >= 2) {
      return { state: APP_STATE.OFFLINE, detail: 'proc exited' };
    }
    const now = nowMs();
    if (isPermissionPromptActive(config_expandHome(cfg.sessions_path || '~/.claude/sessions'))) {
      return { state: APP_STATE.WAITING, detail: 'permission prompt' };
    }

    // 过滤非用户会话：Claude 目录里混有 agent 活性检测（entrypoint=sdk-cli，lastPrompt 为
    // LIVE_OK/pwd 等），它们频繁写 transcript 且状态为 working/error/done，会污染聚合导致
    // 灯疯狂跳变。默认只保留 entrypoint=cli 的用户会话（可经 config.entrypointInclude 覆盖）。
    const entrypointAllow = Array.isArray(cfg.entrypointInclude) ? cfg.entrypointInclude : ['cli'];
    const files = recentFiles(projectsDir, {
      ext: '.jsonl',
      maxDepth: 3,
      windowMs: ACTIVE_WINDOW_MS,
      limit: MAX_SESSIONS,
    }).filter((f) => {
      const ep = readEntrypoint(f.path);
      if (!ep) return true; // 无 entrypoint 信息则保留（老会话）
      return entrypointAllow.includes(ep);
    });
    if (files.length === 0) {
      return {
        state: running ? APP_STATE.IDLE : APP_STATE.OFFLINE,
        detail: 'no transcript',
      };
    }

    const sessions = files.map((f) => ({
      state: this.evaluateTranscript(f.path, f.mtimeMs, now),
      mtimeMs: f.mtimeMs,
    }));
    return this.aggregateBehavior(sessions);
  }
}

// 从 transcript 尾部读取 entrypoint（agent 活性检测会话为 sdk-cli，用户会话为 cli）
function readEntrypoint(filePath) {
  try {
    const lines = tailLines(filePath, 8 * 1024);
    for (let i = lines.length - 1; i >= 0; i--) {
      if (lines[i]?.entrypoint) return lines[i].entrypoint;
    }
  } catch {
    /* 不可读则返回 null */
  }
  return null;
}

module.exports = { ClaudeCollector, isPermissionPromptActive };
