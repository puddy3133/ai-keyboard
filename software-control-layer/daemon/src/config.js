// 配置加载（JSON，避免 YAML 依赖）
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

const CONFIG_DIR = path.join(os.homedir(), '.ai-keyboard-light');
const CONFIG_PATH = path.join(CONFIG_DIR, 'config.json');
const LOG_PATH = path.join(CONFIG_DIR, 'daemon.log');
const PID_PATH = path.join(CONFIG_DIR, 'daemon.pid');

const DEFAULT_CONFIG = {
  lights: {
    L1: 'openclaw',
    L2: 'codex',
    L3: 'claude',
    L4: 'deepseek',
    L5: 'doubaowork',
  },
  collectors: {
    codex: {
      enabled: true,
      sessions_path: '~/.codex/sessions',
      session_index_path: '~/.codex/session_index.jsonl',
      global_state_path: '~/.codex/.codex-global-state.json',
      poll_interval_seconds: 2,
    },
    claude: {
      enabled: true,
      sessions_path: '~/.claude/sessions',
      projects_path: '~/.claude/projects',
      poll_interval_seconds: 2,
    },
    openclaw: {
      enabled: true,
      health_url: 'http://127.0.0.1:18789/health',
      db_path: '~/.openclaw/state/openclaw.sqlite',
      log_path: '~/Library/Logs/openclaw/gateway.log',
      poll_interval_seconds: 2,
    },
    doubaowork: {
      enabled: true,
      app_state_path:
        '~/Library/Application Support/DoubaoWork/saman_app_state',
      indexeddb_path:
        '~/Library/Application Support/DoubaoWork/Default/IndexedDB',
      poll_interval_seconds: 2,
    },
    deepseek: {
      enabled: true,
      port: 3080,
      sessions_path: '~/.dsh/sessions',
      poll_interval_seconds: 2,
    },
    pi: {
      enabled: true,
      sessions_path: '~/.pi/agent/sessions',
      poll_interval_seconds: 2,
    },
  },
  serial: {
    vid: '303a',
    baud: 115200,
    reconnect_interval_seconds: 5,
  },
  ble: {
    enabled: true,
    name: 'EasyInput AI',
  },
  daemon: {
    heartbeat_interval_seconds: 10,
    poll_interval_seconds: 2,
    log_level: 'info',
  },
  http: {
    enabled: true,
    port: 8765,
    host: '127.0.0.1',
  },
};

function expandHome(p) {
  if (typeof p !== 'string') return p;
  if (p === '~') return os.homedir();
  if (p.startsWith('~/')) return path.join(os.homedir(), p.slice(2));
  return p;
}

function loadConfig() {
  let config;
  try {
    const raw = fs.readFileSync(CONFIG_PATH, 'utf8');
    config = JSON.parse(raw);
  } catch (err) {
    // 无配置或损坏：用默认并落盘
    config = JSON.parse(JSON.stringify(DEFAULT_CONFIG));
    try {
      fs.mkdirSync(CONFIG_DIR, { recursive: true });
      fs.writeFileSync(CONFIG_PATH, JSON.stringify(config, null, 2) + '\n', 'utf8');
    } catch (e) {
      // 写配置失败不阻断启动
    }
  }
  // 深度合并默认值，防止新增字段缺失
  return deepMerge(structuredClone(DEFAULT_CONFIG), config);
}

function deepMerge(base, override) {
  const out = Array.isArray(base) ? base.slice() : { ...base };
  if (override && typeof override === 'object') {
    for (const key of Object.keys(override)) {
      const ov = override[key];
      const bv = out[key];
      if (
        ov && typeof ov === 'object' && !Array.isArray(ov) &&
        bv && typeof bv === 'object' && !Array.isArray(bv)
      ) {
        out[key] = deepMerge(bv, ov);
      } else {
        out[key] = ov;
      }
    }
  }
  return out;
}

function saveConfig(config) {
  fs.mkdirSync(CONFIG_DIR, { recursive: true });
  fs.writeFileSync(CONFIG_PATH, JSON.stringify(config, null, 2) + '\n', 'utf8');
}

module.exports = {
  CONFIG_DIR,
  CONFIG_PATH,
  LOG_PATH,
  PID_PATH,
  DEFAULT_CONFIG,
  loadConfig,
  saveConfig,
  expandHome,
};
