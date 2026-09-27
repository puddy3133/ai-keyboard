// Deepseek Harness (dsh) — 进程/端口检测 + session.jsonl.zstd mtime（多会话聚合）
// 来源：dsh web 进程 + TCP 3080 + ~/.dsh/sessions/**/session.jsonl.zstd
// 注意：3080 无 HTTP 状态端点（实测全 404），只用进程/端口/文件 mtime
// v1.11：多会话聚合——每个会话按 mtime 窗口判定（0-30s 工作蓝 / 30-60s 完成绿 / 之后灭），
//        然后按 红>黄>绿>蓝>灭 取最高优先级。无状态变量，守护进程重启不误报。
'use strict';

const fs = require('fs');
const { BaseCollector } = require('./base');
const { APP_STATE } = require('../state');
const { config_expandHome } = require('./path');
const {
  isProcessRunningPattern,
  portListening,
  recentFiles,
  nowMs,
} = require('./util');
// 纯 JS zstd 解压（fzstd），用于解析 dsh session.jsonl.zstd 检测失败标记
const { decompress } = require('fzstd');

const WORKING_WINDOW_MS = 30 * 1000; // 最后写入 30s 内 → 工作中（蓝）
const DONE_HOLD_MS = 60 * 1000;  // 匹配固件 done 动画(绿闪30s+常亮30s)      // 停止后 30s → 完成（绿）
const ACTIVE_WINDOW_MS = 2 * 60 * 1000; // 只聚合 2 分钟内活跃的会话
const MAX_SESSIONS = 8;
const MAX_SESSION_BYTES = 5 * 1024 * 1024; // session 超过 5MB 不解压（避免性能开销）
const ERROR_SCAN_LINES = 40; // 解压后只扫尾部 40 行找错误标记

class DeepseekCollector extends BaseCollector {
  constructor(config) {
    super('deepseek', 'Deepseek Harness', config);
    // 解压结果缓存：filePath → { size, mtimeMs, status }，文件未变化时复用
    this._statusCache = new Map();
  }

  // 解压 session 尾部，根据最后事件判定状态（即时，无需等 mtime 窗口）：
  //   turn/end reason.kind='error' → error；'completed' → done
  //   step/end reason.kind='error' → error
  //   其他活跃事件（chunk/tool/start 等，无 turn/end 收尾）→ working
  readSessionStatus(filePath) {
    let st;
    try {
      st = fs.statSync(filePath);
    } catch {
      return { kind: null };
    }
    const cached = this._statusCache.get(filePath);
    if (cached && cached.size === st.size && cached.mtimeMs === st.mtimeMs) {
      return cached.status;
    }
    let status = { kind: null };
    if (st.size <= MAX_SESSION_BYTES) {
      try {
        const buf = fs.readFileSync(filePath);
        const lines = Buffer.from(decompress(buf))
          .toString('utf8')
          .split('\n')
          .filter((l) => l.trim().length > 0);
        for (let i = lines.length - 1; i >= Math.max(0, lines.length - ERROR_SCAN_LINES); i--) {
          let j;
          try {
            j = JSON.parse(lines[i]);
          } catch {
            continue;
          }
          const t = j.type;
          if (t === 'turn/end') {
            const k = j.data?.reason?.kind;
            // error → 红；aborted/cancelled 等取消类 → 灭；completed/其他 → 绿
            if (k === 'error') {
              status = { kind: 'error' };
            } else if (k === 'aborted' || k === 'cancelled' || k === 'canceled' || k === 'stopped' || k === 'interrupted' || k === 'user') {
              status = { kind: 'idle' };
            } else {
              status = { kind: 'done' };
            }
            break;
          }
          if (t === 'step/end' && j.data?.reason?.kind === 'error') {
            status = { kind: 'error' };
            break;
          }
          // 活跃事件（正在生成/调工具/用户已发消息）→ working
          if (
            t === 'assistant/chunk' || t === 'assistant/message' ||
            t === 'tool/call' || t === 'tool/result' ||
            t === 'turn/start' || t === 'step/start' ||
            t === 'user/message' || t === 'request/header' ||
            t === 'reasoning-chunks' || t === 'text-chunks' || t === 'tool-call-chunks'
          ) {
            status = { kind: 'working' };
            break;
          }
        }
      } catch {
        status = { kind: null }; // 解压失败走 mtime 兜底
      }
    }
    this._statusCache.set(filePath, { size: st.size, mtimeMs: st.mtimeMs, status });
    return status;
  }

  // 评估单个 dsh 会话状态：优先即时事件信号，其次 mtime 窗口兜底
  evaluateSession(filePath, mtimeMs, now) {
    const status = this.readSessionStatus(filePath);
    if (status.kind === 'error') {
      return { state: APP_STATE.ERROR, detail: 'session error' };
    }
    if (status.kind === 'idle') {
      return { state: APP_STATE.IDLE, detail: 'aborted' };
    }
    if (status.kind === 'done') {
      const ageMs = now - mtimeMs;
      // 完成信号出现后保持 DONE（绿闪30s+常亮30s），超过 DONE_HOLD 转 idle
      if (ageMs < DONE_HOLD_MS) {
        return { state: APP_STATE.DONE, detail: 'completed' };
      }
      return { state: APP_STATE.IDLE, detail: 'done expired' };
    }
    if (status.kind === 'working') {
      return { state: APP_STATE.WORKING, detail: 'session active' };
    }
    // 兜底：无事件信号时按 mtime 窗口
    const ageMs = now - mtimeMs;
    if (ageMs < WORKING_WINDOW_MS) {
      return { state: APP_STATE.WORKING, detail: 'session active' };
    }
    if (ageMs < WORKING_WINDOW_MS + DONE_HOLD_MS) {
      return { state: APP_STATE.DONE, detail: 'just stopped' };
    }
    return { state: APP_STATE.IDLE, detail: 'idle' };
  }

  async collect() {
    const cfg = this.config || {};
    const sessionsDir = config_expandHome(cfg.sessions_path);
    const port = Number(cfg.port) || 3080;
    const now = nowMs();

    const procUp = isProcessRunningPattern('dsh');
    const portUp = portListening(port);
    if (!procUp && !portUp) {
      return { state: APP_STATE.OFFLINE, detail: 'no proc/port' };
    }

    const files = recentFiles(sessionsDir, {
      ext: '.zstd',
      maxDepth: 6,
      windowMs: ACTIVE_WINDOW_MS,
      limit: MAX_SESSIONS,
    });
    if (files.length === 0) {
      return { state: APP_STATE.IDLE, detail: 'no session file' };
    }

    const sessions = files.map((f) => ({
      state: this.evaluateSession(f.path, f.mtimeMs, now),
      mtimeMs: f.mtimeMs,
    }));
    return this.aggregateBehavior(sessions);
  }
}

module.exports = { DeepseekCollector };
