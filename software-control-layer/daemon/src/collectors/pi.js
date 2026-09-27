// Pi (PI Coding Agent) 采集器 — 无侵入文件监控（多会话聚合）
// 来源：~/.pi/agent/sessions/<cwd-encoded>/*.jsonl
// 事件：message(user/assistant)，assistant 带 stopReason（stop/end_turn=完成；error/rate_limit=错误）
// v1.11：多会话聚合——扫描最近活跃的会话，各自评估，按 红>黄>绿>蓝>灭 取最高优先级。
'use strict';

const { BaseCollector } = require('./base');
const { APP_STATE } = require('../state');
const { config_expandHome } = require('./path');
const {
  isProcessMatching,
  isProcessRunning,
  recentFiles,
  tailLines,
  nowMs,
} = require('./util');

const DONE_HOLD_MS = 60 * 1000;  // 匹配固件 done 动画(绿闪30s+常亮30s)
const ACTIVE_WINDOW_MS = 15 * 60 * 1000;
const MAX_SESSIONS = 8;

// 正常完成 stopReason
const DONE_STOP_REASONS = new Set(['stop', 'end_turn', 'done', 'finished']);
// 异常 stopReason → error
const ERROR_STOP_REASONS = new Set([
  'error', 'rate_limit', 'max_tokens', 'length', 'abort', 'cancelled', 'cancel', 'content_filter',
]);

function isPiProcessRunning(config) {
  const include = config.procInclude || ['/local/bin/pi'];
  const exclude = config.procExclude || [];
  if (isProcessMatching({ include, exclude })) return true;

  // macOS ps exposes process.title, while pi replaces its original CLI path
  // with "pi" (or the default Greek title "π"). Keep the fallback narrow so
  // custom process matching rules retain their original semantics.
  const usesDefaultMatcher =
    include.length === 1 &&
    include[0] === '/local/bin/pi' &&
    exclude.length === 0;
  return usesDefaultMatcher && (isProcessRunning('pi') || isProcessRunning('\u03c0'));
}

class PiCollector extends BaseCollector {
  constructor(config) {
    super('pi', 'Pi', config);
  }

  // 评估单个 pi 会话状态
  evaluateSession(filePath, mtimeMs, now) {
    const ageMs = now - mtimeMs;
    // 取最后一条 message 事件（跳过 thinking_level_change/session/model_change 等非消息事件）
    const lines = tailLines(filePath);
    let lastMsg = null;
    for (let i = lines.length - 1; i >= 0; i--) {
      const r = lines[i];
      if (r && r.type === 'message') {
        lastMsg = r;
        break;
      }
    }

    const m = (lastMsg && lastMsg.message) || {};
    const role = m.role;
    const stopReason = m.stopReason ? String(m.stopReason).toLowerCase() : '';

    // 错误：assistant 异常 stopReason
    if (role === 'assistant' && stopReason && ERROR_STOP_REASONS.has(stopReason)) {
      return { state: APP_STATE.ERROR, detail: `stop:${m.stopReason}` };
    }

    // 完成：assistant 正常 stopReason（优先于 working；以文件最后写入时刻为基准 30s hold）
    if (role === 'assistant' && stopReason && DONE_STOP_REASONS.has(stopReason)) {
      if (now - mtimeMs <= DONE_HOLD_MS) {
        return { state: APP_STATE.DONE, detail: 'assistant reply' };
      }
      return { state: APP_STATE.IDLE, detail: 'done expired' };
    }

    // 工作中：30s 内有写入（对话进行中 / 流式回复未结束）
    if (ageMs < 30 * 1000) {
      return { state: APP_STATE.WORKING, detail: 'active' };
    }

    // 用户消息结尾 → 等待输入
    if (role === 'user') {
      return { state: APP_STATE.IDLE, detail: 'awaiting input' };
    }

    return { state: APP_STATE.IDLE, detail: 'idle' };
  }

  async collect() {
    const cfg = this.config || {};
    const sessionsDir = config_expandHome(cfg.sessions_path);
    const now = nowMs();

    // 进程门控：pi 进程未运行 → offline（灭灯）；退出后尽快灭（连续 2 轮 ~4s 确认）
    // pi 默认 process.title 是希腊字母 "π"，pgrep -x pi 匹配不到 → 改按命令行路径匹配
    const running = isPiProcessRunning(cfg);
    this._downStreak = running ? 0 : (this._downStreak || 0) + 1;
    if (this._downStreak >= 2) {
      return { state: APP_STATE.OFFLINE, detail: 'proc exited' };
    }

    const files = recentFiles(sessionsDir, {
      ext: '.jsonl',
      maxDepth: 6,
      windowMs: ACTIVE_WINDOW_MS,
      limit: MAX_SESSIONS,
    });
    if (files.length === 0) {
      return { state: running ? APP_STATE.IDLE : APP_STATE.OFFLINE, detail: 'no session' };
    }

    const sessions = files.map((f) => ({
      state: this.evaluateSession(f.path, f.mtimeMs, now),
      mtimeMs: f.mtimeMs,
    }));
    return this.aggregateBehavior(sessions);
  }
}

module.exports = { PiCollector };
