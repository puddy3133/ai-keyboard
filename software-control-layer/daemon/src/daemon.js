// 守护进程主协调器：采集 → 聚合 → 灯映射 → 串口下发
'use strict';

const fs = require('fs');
const path = require('path');
const { CdcController } = require('./serial');
const { BleController } = require('./ble');
const { APP_STATE, STATE_TO_FRAME } = require('./state');
const { CodexCollector } = require('./collectors/codex');
const { ClaudeCollector } = require('./collectors/claude');
const { OpenClawCollector } = require('./collectors/openclaw');
const { DoubaoWorkCollector } = require('./collectors/doubaowork');
const { DeepseekCollector } = require('./collectors/deepseek');
const { PiCollector } = require('./collectors/pi');
const { WorkBuddyCollector } = require('./collectors/workbuddy');
const { LOG_PATH, saveConfig } = require('./config');

const ALL_APPS = ['codex', 'chatgpt', 'claude', 'openclaw', 'doubaowork', 'deepseek', 'pi', 'workbuddy'];

class Daemon {
  constructor(config) {
    this.config = config;
    this.logger = makeLogger(config);
    this.appStates = {}; // appId -> {state, updatedAt, detail}
    this.overrides = {}; // L1..L5 -> frameState (手动覆盖)
    // 运行时灯位映射：启动时从 config 载入，运行中可通过 API 热切换并持久化
    this.lights = { ...(config.lights || {}) };
    // 传输层：USB CDC + BLE 并存，谁连着谁发帧（固件对同一类帧幂等处理）
    this.transports = [new CdcController(config, this.logger)];
    if (config.ble?.enabled !== false) {
      this.transports.push(new BleController(config, this.logger));
    }
    this.collectors = new Map();
    this.http = null;
    this._stopped = false;
    this._lastSent = null;
    this._watchdog = null;
  }

  getLog() {
    return this.logger;
  }

  start() {
    this.logger('info', `ai-keyboard-light starting (pid ${process.pid})`);
    for (const t of this.transports) t.start();

    // 创建并注册采集器（按配置 enabled）
    const builders = {
      codex: (c) => new CodexCollector(c),
      // ChatGPT Desktop 内置 Codex（originator=Codex Desktop），复用 CodexCollector 但按来源过滤独立成灯
      chatgpt: (c) => new CodexCollector({ ...c, appId: 'chatgpt', appName: 'ChatGPT Desktop' }),
      claude: (c) => new ClaudeCollector(c),
      openclaw: (c) => new OpenClawCollector(c),
      doubaowork: (c) => new DoubaoWorkCollector(c),
      deepseek: (c) => new DeepseekCollector(c),
      pi: (c) => new PiCollector(c),
      // WorkBuddy：OpenClaw 内的 Skill 发布工作流，仅监控不占灯位
      workbuddy: (c) => new WorkBuddyCollector(c),
    };
    for (const appId of ALL_APPS) {
      const ccfg = this.config.collectors?.[appId];
      if (!ccfg || ccfg.enabled === false) continue;
      const collector = builders[appId]({ ...ccfg });
      collector.onStateChange((state) => {
        this.appStates[appId] = state;
        this._onAppChange();
      });
      collector.start();
      this.collectors.set(appId, collector);
    }
    this.logger('info', `collectors: ${[...this.collectors.keys()].join(', ')}`);

    // HTTP API（可选）
    const { HttpApi } = require('./httpApi');
    this.http = new HttpApi(this, this.config);
    this.http.start();

    this._startWatchdog();
  }

  // BLE 自动恢复 watchdog：BLE 断开后 noble 可能卡死，重连逻辑（rescanning）
  // 无法自愈；长时间连不上时自动退出进程，由 launchctl KeepAlive 拉起新进程，
  // 新 noble 重新扫描连接。为避免键盘不在场时频繁重启，二次重启间隔拉长。
  _startWatchdog() {
    if (this.config.ble?.enabled === false) return;
    const ble = this.transports.find((t) => t.constructor.name === 'BleController');
    if (!ble) return;
    let lastRestart = 0;
    this._watchdog = setInterval(() => {
      if (this._stopped) return;
      if (ble.isConnected()) return;
      const now = Date.now();
      const disconnectedMs = ble.disconnectedForMs ? ble.disconnectedForMs() : 0;
      if (disconnectedMs <= 0) return;
      // 上次重启后 10 分钟内再次断开 → 可能键盘不在场，拉长阈值避免频繁重启
      const sinceLastRestart = now - lastRestart;
      const threshold = sinceLastRestart < 10 * 60 * 1000 ? 10 * 60 * 1000 : 180 * 1000;
      if (disconnectedMs < threshold) return;
      this.logger('warn', `ble disconnected ${Math.round(disconnectedMs / 1000)}s, restarting daemon to recover noble`);
      lastRestart = now;
      process.exit(0); // launchctl KeepAlive 自动拉起新进程
    }, 30 * 1000);
    if (this._watchdog.unref) this._watchdog.unref();
  }

  stop() {
    this._stopped = true;
    if (this._watchdog) { clearInterval(this._watchdog); this._watchdog = null; }
    if (this.http) this.http.stop();
    for (const c of this.collectors.values()) c.stop();
    for (const t of this.transports) t.stop();
    this.logger('info', 'ai-keyboard-light stopped');
  }

  getAppStates() {
    return { ...this.appStates };
  }

  // 手动覆盖 / 恢复
  setLightOverride(lightId, frameState) {
    this.overrides[lightId] = frameState;
    this._onAppChange();
  }
  clearLightOverride(lightId) {
    delete this.overrides[lightId];
    this._onAppChange();
  }
  clearAllOverrides() {
    this.overrides = {};
    this._onAppChange();
  }

  // 计算 5 个灯的帧状态
  computeLightFrames() {
    const lights = this.lights;
    const frames = [];
    for (let i = 1; i <= 5; i++) {
      const key = `L${i}`;
      if (this.overrides[key] !== undefined) {
        frames.push(this.overrides[key]);
        continue;
      }
      const appId = lights[key];
      if (!appId || !ALL_APPS.includes(appId)) {
        frames.push(0); // 未映射 → 灭
        continue;
      }
      const st = this.appStates[appId]?.state;
      frames.push(STATE_TO_FRAME[st] ?? 0);
    }
    return frames;
  }

  getLightStates() {
    const lights = this.lights;
    const out = {};
    const frames = this.computeLightFrames();
    for (let i = 1; i <= 5; i++) {
      const key = `L${i}`;
      const appId = lights[key];
      const appState = appId ? this.appStates[appId] : undefined;
      out[key] = {
        app: appId || null,
        frame: frames[i - 1],
        state: appState?.state || APP_STATE.OFFLINE,
        overridden: this.overrides[key] !== undefined,
      };
    }
    return out;
  }

  // 当前灯位映射（运行时，热切换）
  getMappings() {
    return { ...this.lights };
  }

  // 热切换灯位映射：校验 → 更新内存 → 写回 config.json 持久化 → 立即重新下发状态帧
  setLightMapping(lightId, appId) {
    if (!/^L[1-5]$/.test(lightId || '')) {
      return { ok: false, error: `invalid light: ${lightId}` };
    }
    if (!appId || !ALL_APPS.includes(appId)) {
      return { ok: false, error: `invalid app: ${appId}` };
    }
    this.lights[lightId] = appId;
    this.config.lights = { ...this.lights }; // 同步到 config 对象
    try {
      saveConfig(this.config);
    } catch (e) {
      this.logger('warn', `setLightMapping saveConfig failed: ${e.message}`);
    }
    this.logger('info', `mapping ${lightId} -> ${appId}`);
    this._onAppChange(); // 立即重算并下发帧（无需重启）
    return { ok: true };
  }

  // 通信通道实时状态（USB CDC / BLE）
  getChannelStates() {
    const out = {
      serial: { connected: false, port: null },
      ble: { connected: false, disconnectedForMs: 0 },
    };
    for (const t of this.transports) {
      const name = t.constructor.name;
      if (name === 'CdcController') {
        out.serial.connected = !!t.isConnected();
        out.serial.port = t.port || null;
      } else if (name === 'BleController') {
        out.ble.connected = !!t.isConnected();
        out.ble.disconnectedForMs = t.disconnectedForMs ? t.disconnectedForMs() : 0;
      }
    }
    return out;
  }

  _onAppChange() {
    if (this._stopped) return;
    const frames = this.computeLightFrames();
    const key = frames.join(',');
    if (key !== this._lastSent) {
      this._lastSent = key;
      this.logger('info', `light → [${key}]`);
    }
    for (const t of this.transports) t.sendStates(frames);
  }
}

function makeLogger(config) {
  const level = config.daemon?.log_level || 'info';
  const order = { debug: 0, info: 1, warn: 2, error: 3 };
  const min = order[level] ?? 1;
  let stream = null;
  try {
    fs.mkdirSync(path.dirname(LOG_PATH), { recursive: true });
    stream = fs.createWriteStream(LOG_PATH, { flags: 'a' });
  } catch {
    stream = null;
  }
  return (lv, msg) => {
    if (order[lv] < min) return;
    const line = `${new Date().toISOString()} [${lv}] ${msg}`;
    if (stream) stream.write(line + '\n');
  };
}

module.exports = { Daemon, makeLogger };
