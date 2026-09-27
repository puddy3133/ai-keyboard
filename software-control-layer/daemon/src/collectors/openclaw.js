// OpenClaw TUI — /health + SQLite 状态库 + gateway.log
// 来源：http://127.0.0.1:18789/health + ~/.openclaw/state/openclaw.sqlite
'use strict';

const fs = require('fs');
const { BaseCollector } = require('./base');
const { APP_STATE } = require('../state');
const { config_expandHome } = require('./path');
const { sqliteQuery, tailTextLines, nowMs } = require('./util');

const DONE_HOLD_MS = 60 * 1000;  // 匹配固件 done 动画(绿闪30s+常亮30s)
const MODEL_ACTIVE_MS = 30 * 1000;
const ERROR_HOLD_MS = 5 * 60 * 1000; // 报错红灯保留窗口（与 codex task failed 一致）
// 判定 working 的最小持续时长：start 发出后需持续这么久仍无 response 才视为真正工作中。
// 后台任务（如 glm-5.3-flash fallback）每 200-300ms 一轮 start→503 快速重试，
// 若不设下限，轮询落在"start 已发、response 未回"间隙会误判蓝灯，导致蓝红交替闪烁。
const MIN_WORKING_MS = 800;

class OpenClawCollector extends BaseCollector {
  constructor(config) {
    super('openclaw', 'OpenClaw', config);
    this._doneSince = 0;
  }

  async _healthOk() {
    const url = this.config?.health_url || 'http://127.0.0.1:18789/health';
    try {
      const ctrl = new AbortController();
      const t = setTimeout(() => ctrl.abort(), 2000);
      const res = await fetch(url, { signal: ctrl.signal });
      clearTimeout(t);
      if (!res.ok) return false;
      const body = await res.json();
      return body?.ok === true || body?.status === 'live';
    } catch {
      return false;
    }
  }

  _gatewayActivity(logPath, now) {
    if (!fs.existsSync(logPath)) return { working: false, done: false, error: false };
    // model-fetch start/response 配对判断：
    //  - working：最后一次 start 尚无对应 response（调用未完成），且未超时
    //  - error：最近一次"硬错误"（≥400 且排除 429 限流）晚于最近成功，且在错误窗口内
    //  - done：最近一次 2xx 成功在完成窗口内
    // 503(No available channel/服务不可用) 属明确失败，判红（重试成功后自动转绿）；
    // 429(限流) 排除：openclaw 会自动重试，判红易误报。
    let lastStart = 0;
    let lastResponse = 0;
    let lastOk = 0;
    let lastErr = 0;
    for (const line of tailTextLines(logPath)) {
      const match = line.match(/^(\d{4}-\d\d-\d\dT[^ ]+)/);
      const timestamp = match ? Date.parse(match[1]) : 0;
      if (line.includes('[provider-transport-fetch] [model-fetch] start ')) {
        if (timestamp > lastStart) lastStart = timestamp;
      } else if (line.includes('[provider-transport-fetch] [model-fetch] response ')) {
        if (timestamp > lastResponse) lastResponse = timestamp;
        const stMatch = line.match(/status=(\d+)/);
        const status = stMatch ? Number(stMatch[1]) : 0;
        if (status >= 200 && status < 300) {
          if (timestamp > lastOk) lastOk = timestamp;
        } else if (status >= 400 && status !== 429) {
          if (timestamp > lastErr) lastErr = timestamp;
        }
      }
    }
    // 有未完成的调用（最后一次 start 晚于最后一次 response）且持续超过
    // MIN_WORKING_MS（排除快速失败的 start-response 对，避免后台重试误判蓝灯），
    // 且未超过 120s 硬超时
    const working =
      lastStart > 0 &&
      lastStart > lastResponse &&
      now - lastStart >= MIN_WORKING_MS &&
      now - lastStart < 120000;
    // 报错：非工作态，最近一次硬错误晚于最近成功，且在错误保留窗口内
    const error = !working && lastErr > lastOk && (now - lastErr) <= ERROR_HOLD_MS;
    const done = !working && !error && lastOk > 0 && (now - lastOk) <= DONE_HOLD_MS;
    return { working, done, error };
  }

  async collect() {
    const cfg = this.config || {};
    const dbPath = config_expandHome(cfg.db_path);
    const logPath = config_expandHome(cfg.log_path);
    const now = nowMs();

    const healthOk = await this._healthOk();
    if (!healthOk) {
      // gateway 忙时 /health 可能短暂失败；先用 gateway.log 降级判断，避免误报离线
      const gateway = this._gatewayActivity(logPath, now);
      if (gateway.working) return { state: APP_STATE.WORKING, detail: 'model call (health busy)' };
      if (gateway.error) return { state: APP_STATE.ERROR, detail: 'model error (health busy)' };
      if (gateway.done) return { state: APP_STATE.DONE, detail: 'model response (health busy)' };
      this._doneSince = 0;
      return { state: APP_STATE.OFFLINE, detail: 'health down' };
    }

    // ---- SQLite（失败则降级为 health + log）----
    let acpError = null;
    let bindingActive = false;
    let sqliteOk = false;
    const states = []; // 多任务/多会话各自评估，最后聚合
    try {
      if (fs.existsSync(dbPath)) {
        // 最近活跃的 ACP 会话（多会话聚合）
        const acpRows = sqliteQuery(
          dbPath,
          "SELECT state, last_activity_at, last_error FROM acp_sessions ORDER BY last_activity_at DESC LIMIT 8"
        ).trim();
        for (const row of acpRows.split('\n')) {
          if (!row.trim()) continue;
          const [st, la, le] = row.split('|');
          const laMs = Number(la) || 0;
          if (st === 'error' || (le && le.length)) {
            states.push({ state: { state: APP_STATE.ERROR, detail: `acp error: ${le || st}` }, mtimeMs: laMs });
          } else if (st === 'waiting') {
            states.push({ state: { state: APP_STATE.WAITING, detail: 'acp waiting' }, mtimeMs: laMs });
          } else if (st === 'running') {
            states.push({ state: { state: APP_STATE.WORKING, detail: 'acp running' }, mtimeMs: laMs });
          }
          if (st === 'error') acpError = le || st;
        }
        // 最近任务（多任务聚合）
        const taskRows = sqliteQuery(
          dbPath,
          "SELECT status, started_at, ended_at, error FROM task_runs ORDER BY started_at DESC LIMIT 8"
        ).trim();
        for (const row of taskRows.split('\n')) {
          if (!row.trim()) continue;
          const [st, started, ended, err] = row.split('|');
          const startedMs = Number(started) || 0;
          const endedMs = Number(ended) || 0;
          if (st === 'failed' && endedMs > 0 && now - endedMs < 5 * 60 * 1000) {
            states.push({ state: { state: APP_STATE.ERROR, detail: 'task failed' }, mtimeMs: endedMs });
          } else if (st === 'succeeded' && endedMs > 0 && now - endedMs < DONE_HOLD_MS) {
            states.push({ state: { state: APP_STATE.DONE, detail: 'task succeeded' }, mtimeMs: endedMs });
          } else if (st === 'running') {
            states.push({ state: { state: APP_STATE.WORKING, detail: 'task running' }, mtimeMs: startedMs || now });
          }
        }
        const bindings = sqliteQuery(
          dbPath,
          "SELECT COUNT(*) FROM current_conversation_bindings WHERE status != 'completed'"
        ).trim();
        bindingActive = Number(bindings) > 0;
        if (bindingActive) {
          states.push({ state: { state: APP_STATE.WAITING, detail: 'binding active' }, mtimeMs: now });
        }
        sqliteOk = true;
      }
    } catch {
      sqliteOk = false;
    }

    // gateway.log 是普通文本；仅把最近仍未收到 response 的调用视为工作中。
    const gateway = this._gatewayActivity(logPath, now);
    if (gateway.working) {
      states.push({ state: { state: APP_STATE.WORKING, detail: 'model call' }, mtimeMs: now });
    } else if (gateway.error) {
      states.push({ state: { state: APP_STATE.ERROR, detail: 'model error' }, mtimeMs: now });
    } else if (gateway.done) {
      states.push({ state: { state: APP_STATE.DONE, detail: 'model response' }, mtimeMs: now });
    }

    // ---- 行为驱动聚合（waiting 全局阻塞优先；其余按最近活跃会话时间近因）----
    if (sqliteOk && states.length > 0) {
      return this.aggregateBehavior(states);
    }
    // 降级：health 在线但 DB 不可读 → 按 log 判断
    if (gateway.working) {
      return { state: APP_STATE.WORKING, detail: 'model call' };
    }
    if (gateway.error) {
      return { state: APP_STATE.ERROR, detail: 'model error' };
    }
    if (gateway.done) {
      return { state: APP_STATE.DONE, detail: 'model response' };
    }
    return { state: APP_STATE.IDLE, detail: 'idle' };
  }
}

module.exports = { OpenClawCollector };
