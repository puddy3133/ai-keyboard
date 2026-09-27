// BLE 传输层控制器：扫描 → 连接 → 写 0x05 多灯特征 → 心跳保活 → 自动重连
// 使用 @abandonware/noble（macOS CoreBluetooth 后端）
'use strict';

const noble = require('@abandonware/noble');
const { buildStatusFrame, buildHeartbeatFrame } = require('./frame');

const CONFIG_SVC = '7d2f4d106b6f4a2d8b016d4653320001'; // Config 服务（无连字符小写）
const MULTI_WRITE_SUFFIX = '0005'; // 多灯特征 uuid 后缀

class BleController {
  constructor(config, log) {
    this.config = config || {};
    this.log = log || (() => {});
    this.targetName = this.config.name || 'EasyInput AI';
    this.peripheral = null;
    this.writeChar = null;
    this._connected = false;
    this._connecting = false;
    this._scanning = false;
    this._stopped = false;
    this._heartbeatTimer = null;
    this._scanRefreshTimer = null;
    this._disconnectedSince = 0; // BLE 断开时刻（watchdog 用，0=未断开）
    this._lastFrame = buildStatusFrame([0, 0, 0, 0, 0]);
    this._heartbeatMs =
      (Number(this.config.daemon?.heartbeat_interval_seconds) || 10) * 1000;
    this._boundState = null;
    this._boundDiscover = null;
  }

  start() {
    this._stopped = false;
    this._bindEvents();
    if (noble.state === 'poweredOn') this._ensureScanning();
    this._heartbeatTimer = setInterval(() => {
      this._write(buildHeartbeatFrame());
    }, this._heartbeatMs);
    if (this._heartbeatTimer.unref) this._heartbeatTimer.unref();

    // 断开状态下周期性刷新扫描，防止 macOS CoreBluetooth 长时间空扫描
    // discover 静默失效（noble issue #79）。键盘关机期间空扫数小时后，
    // 键盘回来也会扫不到；每 60s 强制 stop+start 一次即可根治。
    const refreshMs =
      (Number(this.config.ble?.scan_refresh_seconds) || 60) * 1000;
    this._scanRefreshTimer = setInterval(() => {
      if (this._connected || this._stopped) return;
      if (this._scanning) this._refreshScan();
    }, refreshMs);
    if (this._scanRefreshTimer.unref) this._scanRefreshTimer.unref();
  }

  stop() {
    this._stopped = true;
    if (this._heartbeatTimer) clearInterval(this._heartbeatTimer);
    this._heartbeatTimer = null;
    if (this._scanRefreshTimer) clearInterval(this._scanRefreshTimer);
    this._scanRefreshTimer = null;
    if (this._boundState) noble.removeListener('stateChange', this._boundState);
    if (this._boundDiscover) noble.removeListener('discover', this._boundDiscover);
    try {
      if (this._scanning) noble.stopScanningAsync();
    } catch {}
    try {
      if (this.peripheral) this.peripheral.disconnectAsync();
    } catch {}
    this.peripheral = null;
    this.writeChar = null;
    this._connected = false;
    this._connecting = false;
    this._scanning = false;
  }

  isConnected() {
    return this._connected;
  }

  // BLE 断开持续时长（毫秒）；未断开返回 0（watchdog 用）
  disconnectedForMs() {
    if (this._connected) return 0;
    return this._disconnectedSince ? Date.now() - this._disconnectedSince : 0;
  }

  // 下发 5 灯状态帧
  sendStates(states) {
    this._lastFrame = buildStatusFrame(states);
    this._write(this._lastFrame);
  }

  _bindEvents() {
    this._boundState = async (state) => {
      if (state === 'poweredOn') this._ensureScanning();
      else if (this._connected) {
        this._connected = false;
        this.peripheral = null;
        this.writeChar = null;
        this.log('warn', `ble power off (${state})`);
      }
    };
    noble.on('stateChange', this._boundState);

    this._boundDiscover = async (peripheral) => {
      if (this._connected || this._connecting || this._stopped) return;
      const name = peripheral.advertisement?.localName;
      const uuids = peripheral.advertisement?.serviceUuids || [];
      const matched =
        name === this.targetName ||
        uuids.some((u) => u.toLowerCase().includes('7d2f4d10'));
      if (!matched) return;
      this.log('info', `ble device found: ${name} (${peripheral.id})`);
      this._connect(peripheral);
    };
    noble.on('discover', this._boundDiscover);
  }

  async _ensureScanning() {
    if (this._connected || this._stopped || this._scanning) return;
    try {
      await noble.startScanningAsync([], false);
      this._scanning = true;
      this.log('info', 'ble scanning started');
    } catch (err) {
      this.log('warn', `ble scan error: ${err.message}`);
    }
  }

  // 重启扫描（stop + start），刷新 CoreBluetooth 扫描状态，
  // 防止长时间扫描后 discover 静默失效（noble issue #79）
  async _refreshScan() {
    // stop/start 偶发卡住，加超时保护，避免刷新 timer 挂死
    const withTimeout = (p, ms) =>
      Promise.race([p, new Promise((r) => setTimeout(() => r(null), ms))]);
    try {
      if (this._scanning) {
        await withTimeout(noble.stopScanningAsync(), 3000);
        this._scanning = false;
      }
      await withTimeout(noble.startScanningAsync([], false), 5000);
      this._scanning = true;
      this.log('debug', 'ble scan refreshed (anti-stall watchdog)');
    } catch (err) {
      this.log('warn', `ble scan refresh error: ${err.message}`);
    }
  }

  async _connect(peripheral) {
    if (this._connected || this._connecting || this._stopped) return;
    this._connecting = true;
    try {
      await noble.stopScanningAsync();
      this._scanning = false;
      try {
        await peripheral.connectAsync();
        this.log('info', 'ble connected');
      } catch (err) {
        // CoreBluetooth 可能残留旧连接：长时间断开后键盘重新广播，noble discover 到它，
        // 但 connectAsync 报 "Peripheral already connected"（系统缓存认为已连接），
        // 而守护进程侧 _connected=false → 帧被 _write 丢弃、灯不生效，且若放弃会陷入
        // discover→already connected→rescan 死循环。正确做法：复用现有连接继续恢复。
        if (!/already connected/i.test(err.message)) throw err;
        this.log('warn', 'ble already connected, reusing existing connection');
      }
      await this._finishConnect(peripheral);
    } catch (err) {
      this._connecting = false;
      this.log('warn', `ble connect error: ${err.message}`);
      this.peripheral = null;
      this.writeChar = null;
      this._connected = false;
      this._ensureScanning();
    }
  }

  // 连接（或复用已连接外设）后：发现服务/特征、建立写通道、立即下发最近状态帧
  async _finishConnect(peripheral) {
    try {
      await peripheral.discoverAllServicesAndCharacteristicsAsync();

      // 找多灯写特征（uuid 后缀 0005）
      let writeChar = null;
      for (const svc of peripheral.services || []) {
        for (const c of svc.characteristics || []) {
          if (c.uuid.toLowerCase().endsWith(MULTI_WRITE_SUFFIX)) {
            writeChar = c;
            break;
          }
        }
        if (writeChar) break;
      }
      if (!writeChar) {
        this.log('warn', 'ble: 0x05 multi-agent characteristic not found (firmware not updated?)');
        try { peripheral.disconnectAsync(); } catch {}
        this._connecting = false;
        return;
      }

      this.peripheral = peripheral;
      this.writeChar = writeChar;
      this._connected = true;
      this._connecting = false;
      this._disconnectedSince = 0;
      peripheral.once('disconnect', () => this._onDisconnected());
      this.log('info', `ble multi-agent channel ready (${writeChar.uuid})`);
      // 连接（或复用）后立即下发最近状态帧，覆盖长时间断开期间的积压状态
      this._write(this._lastFrame);
    } catch (err) {
      throw err;
    }
  }

  _write(buf) {
    if (!this._connected || !this.writeChar) return;
    try {
      // withoutResponse=true → writeWithoutResponse（0x05 支持，无需等待响应）
      this.writeChar.writeAsync(buf, true).catch((err) => {
        this.log('warn', `ble write error: ${err.message}`);
      });
    } catch (err) {
      this.log('warn', `ble write exception: ${err.message}`);
    }
  }

  _onDisconnected() {
    if (!this._connected) return;
    this._connected = false;
    this._connecting = false;
    this.peripheral = null;
    this.writeChar = null;
    this._disconnectedSince = Date.now();
    this.log('warn', 'ble disconnected, rescanning');
    this._ensureScanning();
  }
}

module.exports = { BleController };
