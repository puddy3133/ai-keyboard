#!/usr/bin/env node
// AI 键盘灯控 CLI：kb-light
'use strict';

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const { loadConfig, CONFIG_DIR, LOG_PATH, PID_PATH } = require('./src/config');

const API_BASE = 'http://127.0.0.1:8765';

async function httpJson(method, urlPath, body) {
  const url = `${API_BASE}${urlPath}`;
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), 2500);
  try {
    const res = await fetch(url, {
      method,
      signal: ctrl.signal,
      headers: body ? { 'Content-Type': 'application/json' } : undefined,
      body: body ? JSON.stringify(body) : undefined,
    });
    clearTimeout(t);
    return { ok: res.ok, status: res.status, body: await res.json().catch(() => ({})) };
  } catch (e) {
    clearTimeout(t);
    return { ok: false, status: 0, body: { error: 'daemon not reachable' } };
  }
}

function isRunning() {
  try {
    const pid = Number(fs.readFileSync(PID_PATH, 'utf8').trim());
    if (!pid) return false;
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

function cmdStart() {
  if (isRunning()) {
    console.log('already running');
    return;
  }
  fs.mkdirSync(CONFIG_DIR, { recursive: true });
  const child = spawn(process.execPath, [path.join(__dirname, 'kb-light.js'), 'run'], {
    detached: true,
    stdio: 'ignore',
  });
  fs.writeFileSync(PID_PATH, String(child.pid), 'utf8');
  child.unref();
  console.log(`started pid ${child.pid} (log: ${LOG_PATH})`);
}

function cmdRun() {
  const config = loadConfig();
  fs.mkdirSync(CONFIG_DIR, { recursive: true });
  fs.writeFileSync(PID_PATH, `${process.pid}\n`, 'utf8');
  const { Daemon } = require('./src/daemon');
  const daemon = new Daemon(config);
  daemon.start();
  const shutdown = () => {
    daemon.stop();
    try { fs.rmSync(PID_PATH, { force: true }); } catch {}
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
  process.on('SIGHUP', shutdown);
}

function cmdStop() {
  try {
    const pid = Number(fs.readFileSync(PID_PATH, 'utf8').trim());
    if (pid) {
      try { process.kill(pid, 'SIGTERM'); } catch {}
      fs.rmSync(PID_PATH, { force: true });
      console.log(`stopped pid ${pid}`);
    } else {
      console.log('not running');
    }
  } catch {
    console.log('not running');
  }
}

async function cmdStatus() {
  const r = await httpJson('GET', '/api/status');
  if (!r.ok) {
    console.log('daemon 未运行（用 kb-light start 启动）');
    return;
  }
  const { apps, lights } = r.body;
  console.log('应用状态:');
  for (const [app, st] of Object.entries(apps || {})) {
    console.log(`  ${app.padEnd(12)} ${String(st.state).padEnd(8)} ${st.detail || ''} @${new Date(st.updatedAt).toLocaleTimeString()}`);
  }
  console.log('灯状态:');
  for (let i = 1; i <= 5; i++) {
    const L = lights?.[`L${i}`];
    if (!L) continue;
    const ov = L.overridden ? ' [手动]' : '';
    console.log(`  L${i} → ${String(L.app).padEnd(12)} frame=${L.frame} ${L.state}${ov}`);
  }
}

async function cmdSet(light, state) {
  const r = await httpJson('POST', `/api/lights/${light}`, { state });
  if (!r.ok) {
    console.log(`失败: ${r.body?.error || r.status}`);
    return;
  }
  console.log(`${light} → ${state}`);
}

function help() {
  console.log(`kb-light — AI 键盘灯控

用法:
  kb-light start          启动守护进程（后台）
  kb-light stop           停止守护进程
  kb-light status         查看应用与灯状态
  kb-light apps           查看应用状态
  kb-light set L1 working 手动设置灯状态 (working/waiting/done/error/idle/offline)
  kb-light auto L1        恢复该灯自动采集
  kb-light off            一键全灭（下次状态变化自动恢复）
  kb-light test           自检：5 灯依次播放灯效
  kb-light map L1 codex   修改灯映射 (codex/chatgpt/claude/openclaw/doubaowork/deepseek/pi)
  kb-light mapping        查看当前映射
  kb-light reload         重载配置（需重启）
`);
}

async function main(argv) {
  const cmd = argv[0];
  if (!cmd || cmd === 'help' || cmd === '-h' || cmd === '--help') return help();

  switch (cmd) {
    case 'start': return cmdStart();
    case 'run': return cmdRun();
    case 'stop': return cmdStop();
    case 'status':
    case 'apps': return cmdStatus();
    case 'set': {
      const light = argv[1];
      const state = argv[2];
      if (!/^L[1-5]$/.test(light || '') || !state) {
        return console.log('用法: kb-light set L1 working');
      }
      return cmdSet(light, state);
    }
    case 'auto': {
      const light = argv[1];
      if (!/^L[1-5]$/.test(light || '')) return console.log('用法: kb-light auto L1');
      return cmdSet(light, 'auto');
    }
    case 'off': {
      // 全灭：对 5 个灯都发送 idle（下次状态变化自动恢复）
      for (let i = 1; i <= 5; i++) await cmdSet(`L${i}`, 'idle');
      return;
    }
    case 'test': {
      const seq = ['working', 'waiting', 'done', 'error', 'idle'];
      for (const st of seq) {
        console.log(`→ ${st}`);
        for (let i = 1; i <= 5; i++) await cmdSet(`L${i}`, st);
        await new Promise((r) => setTimeout(r, 1500));
      }
      for (let i = 1; i <= 5; i++) await cmdSet(`L${i}`, 'auto');
      return;
    }
    case 'map': {
      const light = argv[1];
      const app = argv[2];
      const valid = ['codex', 'chatgpt', 'claude', 'openclaw', 'doubaowork', 'deepseek', 'pi'];
      if (!/^L[1-5]$/.test(light || '') || !valid.includes(app)) {
        return console.log('用法: kb-light map L1 codex');
      }
      const config = loadConfig();
      config.lights[light] = app;
      const { saveConfig } = require('./src/config');
      saveConfig(config);
      console.log(`${light} → ${app}（已保存，需 restart 生效）`);
      return;
    }
    case 'mapping': {
      const config = loadConfig();
      for (let i = 1; i <= 5; i++) console.log(`L${i} → ${config.lights[`L${i}`]}`);
      return;
    }
    case 'reload':
      console.log('配置重载需重启：kb-light restart（先 stop 再 start）');
      return;
    default:
      return help();
  }
}

main(process.argv.slice(2));
