// HTTP API 服务（默认 127.0.0.1:8765）
'use strict';

const http = require('http');
const { STATE_TO_FRAME, APP_STATE } = require('./state');

class HttpApi {
  constructor(daemon, config) {
    this.daemon = daemon;
    this.config = config;
    this.server = null;
  }

  start() {
    const hc = this.config.http;
    if (!hc || hc.enabled === false) return;
    this.server = http.createServer((req, res) => this._handle(req, res));
    this.server.listen(hc.port, hc.host, () => {
      this.daemon.getLog()('info', `http api on http://${hc.host}:${hc.port}`);
    });
  }

  stop() {
    if (this.server) {
      try { this.server.close(); } catch {}
      this.server = null;
    }
  }

  _json(res, code, obj) {
    const body = JSON.stringify(obj);
    res.writeHead(code, {
      'Content-Type': 'application/json; charset=utf-8',
      'Content-Length': Buffer.byteLength(body),
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type',
    });
    res.end(body);
  }

  _handle(req, res) {
    const url = new URL(req.url, 'http://127.0.0.1');
    const p = url.pathname;
    if (req.method === 'OPTIONS') {
      return this._json(res, 204, {});
    }
    try {
      if (req.method === 'GET' && p === '/api/health') {
        return this._json(res, 200, { status: 'running' });
      }
      if (req.method === 'GET' && p === '/api/status') {
        return this._json(res, 200, {
          apps: this.daemon.getAppStates(),
          lights: this.daemon.getLightStates(),
          channels: this.daemon.getChannelStates(),
        });
      }
      if (req.method === 'GET' && p === '/api/lights') {
        return this._json(res, 200, { lights: this.daemon.getLightStates() });
      }
      // 灯位映射（运行时热切换，无需重启）
      if (req.method === 'GET' && p === '/api/mapping') {
        return this._json(res, 200, { mappings: this.daemon.getMappings() });
      }
      // POST /api/mapping  { "light": "L1", "app": "claude" } → 立即生效并持久化
      if (req.method === 'POST' && p === '/api/mapping') {
        let body = '';
        req.on('data', (c) => (body += c));
        req.on('end', () => {
          try {
            const data = JSON.parse(body || '{}');
            const r = this.daemon.setLightMapping(data.light, data.app);
            if (!r.ok) return this._json(res, 400, { error: r.error });
            this._json(res, 200, { ok: true, mappings: this.daemon.getMappings() });
          } catch (e) {
            this._json(res, 400, { error: e.message });
          }
        });
        return;
      }
      // POST /api/lights/L1  { "state": "working" | "auto" }
      const m = p.match(/^\/api\/lights\/(L[1-5])$/);
      if (m && req.method === 'POST') {
        let body = '';
        req.on('data', (c) => (body += c));
        req.on('end', () => {
          try {
            const data = JSON.parse(body || '{}');
            const light = m[1];
            if (data.state === 'auto' || data.state === 'off') {
              this.daemon.clearLightOverride(light);
            } else if (data.state && STATE_TO_FRAME[data.state] !== undefined) {
              this.daemon.setLightOverride(light, STATE_TO_FRAME[data.state]);
            } else {
              return this._json(res, 400, { error: 'invalid state' });
            }
            this._json(res, 200, { ok: true, light, state: data.state });
          } catch (e) {
            this._json(res, 400, { error: e.message });
          }
        });
        return;
      }
      if (req.method === 'GET' && p === '/api/config') {
        return this._json(res, 200, { lights: this.daemon.config.lights });
      }
      this._json(res, 404, { error: 'not found' });
    } catch (e) {
      this._json(res, 500, { error: e.message });
    }
  }
}

module.exports = { HttpApi };
