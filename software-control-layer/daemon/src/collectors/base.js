// 采集器基类：定时轮询 + 状态去重回调 + 多会话优先级聚合
'use strict';

const { APP_STATE, STATE_PRIORITY } = require('../state');

class BaseCollector {
  constructor(appId, appName, config) {
    this.appId = appId;
    this.appName = appName;
    this.config = config;
    this._timer = null;
    this._listener = null;
    this._collecting = false;
    this._state = { state: APP_STATE.OFFLINE, updatedAt: 0, detail: undefined };
  }

  // 多会话聚合：对每个会话评估出的状态，取优先级最高的（红>黄>绿>蓝>灭）。
  // states: [{state, detail?}, ...]，返回聚合后的 {state, detail}。
  aggregate(states) {
    if (!Array.isArray(states) || states.length === 0) {
      return { state: APP_STATE.IDLE, detail: 'no session' };
    }
    let best = null;
    let bestPriority = -1;
    let bestDetail = undefined;
    for (const s of states) {
      if (!s || !s.state) continue;
      const p = STATE_PRIORITY[s.state] ?? -1;
      if (p > bestPriority) {
        bestPriority = p;
        best = s.state;
        bestDetail = s.detail;
      }
    }
    return { state: best ?? APP_STATE.IDLE, detail: bestDetail };
  }

  // 行为驱动聚合（用户确认，v1.15 起统一）：
  //  - waiting（待确认/待批准）：阻塞性信号，任一会话存在即全局提示，不随切换消失、不设有效期
  //  - 其余（工作/完成/错误/待命）：按"最近活跃会话"时间近因——灯反映用户当前关注点；
  //    错误只在用户停留在报错会话时保留，切走即让位给当前会话，该会话重新活跃时恢复
  // sessions: [{ state: {state, detail?}, mtimeMs: number }]
  aggregateBehavior(sessions) {
    if (!Array.isArray(sessions) || sessions.length === 0) {
      return { state: APP_STATE.IDLE, detail: 'no session' };
    }
    const waiting = sessions.find((s) => s && s.state && s.state.state === APP_STATE.WAITING);
    if (waiting) return waiting.state;
    const sorted = sessions
      .filter((s) => s && s.state && s.state.state)
      .sort((a, b) => (b.mtimeMs || 0) - (a.mtimeMs || 0));
    return sorted[0] ? sorted[0].state : { state: APP_STATE.IDLE, detail: 'no session' };
  }

  // 子类实现：返回 { state, detail? }
  // eslint-disable-next-line no-unused-vars
  async collect() {
    return { state: APP_STATE.OFFLINE };
  }

  getState() {
    return { ...this._state };
  }

  onStateChange(cb) {
    this._listener = cb;
  }

  start() {
    const intervalMs = Math.max(1, Number(this.config?.poll_interval_seconds) || 2) * 1000;
    this._stopTimer();
    // 立即采集一次
    this._tick();
    // 注意：不要 unref 这个 timer！若 event loop 中只剩 unref 的 timer +
    // 活跃 I/O（如 HTTP server listen），poll 阶段会无限阻塞等待 fd，
    // unref timer 永远不触发，导致所有采集器停摆（曾实测 event loop 100%
    // 卡在 kevent，updatedAt 永不更新）。
    this._timer = setInterval(() => this._tick(), intervalMs);
  }

  stop() {
    this._stopTimer();
  }

  _stopTimer() {
    if (this._timer) {
      clearInterval(this._timer);
      this._timer = null;
    }
  }

  async _tick() {
    if (this._collecting) return;
    this._collecting = true;
    let result;
    try {
      result = await this.collect();
    } catch (err) {
      result = { state: APP_STATE.ERROR, detail: `collect error: ${err.message}` };
    } finally {
      this._collecting = false;
    }
    if (!result || !result.state) return;
    // Details are diagnostic data. Only a state transition needs to touch the
    // hardware; otherwise a changing error/detail string can cause write storms.
    const changed =
      this._state.updatedAt === 0 ||
      result.state !== this._state.state;
    this._state = {
      state: result.state,
      updatedAt: Date.now(),
      detail: result.detail,
    };
    if (changed && typeof this._listener === 'function') {
      this._listener(this.getState());
    }
  }
}

module.exports = { BaseCollector };
