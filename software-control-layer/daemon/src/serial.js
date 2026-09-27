// CDC 串口控制器：设备发现、打开、写帧、心跳保活、自动重连
'use strict';

const { SerialPort } = require('serialport');
const { buildStatusFrame, buildHeartbeatFrame } = require('./frame');

class CdcController {
  constructor(config, log) {
    this.config = config || {};
    this.log = log || (() => {});
    this.port = null;
    this._heartbeatTimer = null;
    this._reconnectTimer = null;
    this._opening = false;
    this._stopped = false;
    this._lastErrorAt = 0;
    this._lastErrorMessage = '';
    this._reconnectIntervalMs =
      (Number(this.config.reconnect_interval_seconds) || 5) * 1000;
    this._heartbeatMs =
      (Number(this.config.daemon?.heartbeat_interval_seconds) || 10) * 1000;
    this._lastFrame = buildStatusFrame([0, 0, 0, 0, 0]);
    this._connected = false;
  }

  start() {
    this._stopped = false;
    this._scheduleReconnect(true);
    this._startHeartbeat();
  }

  stop() {
    this._stopped = true;
    if (this._heartbeatTimer) clearInterval(this._heartbeatTimer);
    if (this._reconnectTimer) clearTimeout(this._reconnectTimer);
    this._tryClose();
    this._heartbeatTimer = null;
    this._reconnectTimer = null;
  }

  isConnected() {
    return this._connected;
  }

  // 下发 5 灯状态帧（states: array[5] of 0..4）
  sendStates(states) {
    this._lastFrame = buildStatusFrame(states);
    this._write(this._lastFrame);
  }

  _write(buf) {
    if (!this._connected || !this.port || !this.port.isOpen) {
      return;
    }
    try {
      this.port.write(buf, (err) => {
        if (err) this._logError(err);
      });
    } catch (err) {
      this._logError(err);
    }
  }

  _logError(err) {
    const message = err?.message || String(err);
    const now = Date.now();
    if (message === this._lastErrorMessage && now - this._lastErrorAt < 5000) return;
    this._lastErrorMessage = message;
    this._lastErrorAt = now;
    this.log('warn', `serial error: ${message}`);
  }

  _startHeartbeat() {
    this._heartbeatTimer = setInterval(() => {
      // 纯心跳帧：仅刷新固件 TTL，不改灯状态
      this._write(buildHeartbeatFrame());
    }, this._heartbeatMs);
    if (this._heartbeatTimer.unref) this._heartbeatTimer.unref();
  }

  async _open() {
    if (this._stopped || this._connected || this._opening) return;
    this._opening = true;
    let target = null;
    try {
      const ports = await SerialPort.list();
      const vid = (this.config.vid || '303a').toLowerCase();
      for (const p of ports) {
        const pvid = String(p.vendorId || '').replace(/^0x/, '').toLowerCase();
        if (pvid === vid) {
          target = p;
          break;
        }
        if (/cu\.usbmodem/i.test(p.path)) {
          target = p;
          break;
        }
        if (p.manufacturer && /EasyInput|AIOTWAN/i.test(p.manufacturer)) {
          target = p;
          break;
        }
      }
    } catch (err) {
      this._opening = false;
      this._logError(err);
      return;
    }
    if (!target) {
      this._opening = false;
      this.log('debug', 'no keyboard CDC port found');
      return;
    }

    const port = new SerialPort({
      path: target.path,
      baudRate: Number(this.config.baud) || 115200,
    });
    port.on('open', () => {
      this._opening = false;
      if (this._stopped) {
        try { port.close(); } catch {}
        return;
      }
      this._connected = true;
      this.port = port;
      this.log('info', `serial open: ${target.path}`);
      // 打开后立即下发最近状态帧
      this._write(this._lastFrame);
    });
    port.on('close', () => {
      this._opening = false;
      if (this.port === port) {
        this._connected = false;
        this.port = null;
      }
      if (!this._stopped) {
        this.log('warn', `serial closed: ${target.path}`);
        this._scheduleReconnect();
      }
    });
    port.on('error', (err) => {
      this._logError(err);
      if (this.port === port) {
        this._connected = false;
        this.port = null;
      }
      if (port.isOpen) {
        try { port.close(); } catch {}
      } else {
        this._opening = false;
        this._scheduleReconnect();
      }
    });
  }

  _tryClose() {
    if (this.port && this.port.isOpen) {
      try {
        this.port.close();
      } catch {}
    }
    this.port = null;
    this._connected = false;
  }

  _scheduleReconnect(immediate = false) {
    if (this._stopped || this._reconnectTimer) return;
    this._reconnectTimer = setTimeout(() => {
      this._reconnectTimer = null;
      this._open().then(() => {
        if (!this._connected && !this._opening) this._scheduleReconnect();
      });
    }, immediate ? 200 : this._reconnectIntervalMs);
    if (this._reconnectTimer.unref) this._reconnectTimer.unref();
  }
}

module.exports = { CdcController };
