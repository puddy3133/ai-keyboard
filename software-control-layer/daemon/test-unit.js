// daemon 聚合与帧构造单元测试
'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { loadConfig } = require('./src/config');
const { Daemon } = require('./src/daemon');
const { BaseCollector } = require('./src/collectors/base');
const { CodexCollector } = require('./src/collectors/codex');
const { isPermissionPromptActive } = require('./src/collectors/claude');
const { OpenClawCollector } = require('./src/collectors/openclaw');
const { buildStatusFrame, buildHeartbeatFrame, isValidFrame } = require('./src/frame');

function testFrame() {
  const f = buildStatusFrame([0, 1, 2, 3, 4]);
  assert.strictEqual(f.length, 8, 'frame len');
  assert.strictEqual(f[0], 0x16, 'magic');
  assert.strictEqual(f[1], 0x01, 'version');
  assert.deepStrictEqual([...f.slice(2, 7)], [0, 1, 2, 3, 4], 'states');
  assert.strictEqual(f[7], 0, 'flags');
  // 越界钳制
  const f2 = buildStatusFrame([9, -1, 2, 3, 4]);
  assert.deepStrictEqual([...f2.slice(2, 7)], [4, 0, 2, 3, 4], 'clamp');
  // 心跳
  const hb = buildHeartbeatFrame();
  assert.strictEqual(hb[7], 0x01, 'heartbeat flag');
  assert.ok(isValidFrame(f) && isValidFrame(hb), 'valid');
  console.log('✓ frame 构造/钳制/心跳 OK');
}

function testAggregation() {
  const config = loadConfig();
  const daemon = new Daemon(config);
  // mock appStates
  daemon.appStates = {
    codex: { state: 'working', updatedAt: Date.now(), detail: 't' },
    claude: { state: 'done', updatedAt: Date.now(), detail: 't' },
    openclaw: { state: 'waiting', updatedAt: Date.now(), detail: 't' },
    doubaowork: { state: 'error', updatedAt: Date.now(), detail: 't' },
    deepseek: { state: 'idle', updatedAt: Date.now(), detail: 't' },
  };
  // 以当前配置映射计算期望值，避免测试绑定历史灯位顺序。
  const expected = [];
  for (let i = 1; i <= 5; i++) {
    const app = config.lights[`L${i}`];
    expected.push({
      working: 1,
      done: 3,
      waiting: 2,
      error: 4,
      idle: 0,
    }[daemon.appStates[app]?.state] ?? 0);
  }
  const frames = daemon.computeLightFrames();
  assert.deepStrictEqual(frames, expected, `当前映射帧: ${frames}`);
  console.log('✓ 缺省映射聚合 →', frames.join(','));

  // 手动覆盖
  daemon.setLightOverride('L1', 0);
  const f2 = daemon.computeLightFrames();
  assert.strictEqual(f2[0], 0, 'L1 override');
  daemon.clearLightOverride('L1');
  const f3 = daemon.computeLightFrames();
  assert.strictEqual(f3[0], expected[0], 'L1 restore');
  console.log('✓ 手动覆盖/恢复 OK');

  // 未映射灯 → 灭
  const appForL1 = config.lights.L1;
  daemon.appStates[appForL1] = undefined;
  const f4 = daemon.computeLightFrames();
  assert.strictEqual(f4[0], 0, 'unmapped -> off');
  console.log('✓ 未映射灯 → 灭 OK');
}

async function testCollectorTickGuards() {
  let calls = 0;
  let notifications = 0;
  class FakeCollector extends BaseCollector {
    async collect() {
      calls++;
      await new Promise((resolve) => setTimeout(resolve, 30));
      return { state: 'working', detail: `tick ${calls}` };
    }
  }
  const collector = new FakeCollector('fake', 'Fake', { poll_interval_seconds: 2 });
  collector.onStateChange(() => notifications++);
  const first = collector._tick();
  const second = collector._tick();
  await Promise.all([first, second]);
  assert.strictEqual(calls, 1, 'overlapping collect calls must be suppressed');
  assert.strictEqual(notifications, 1, 'same state must notify only once');
  assert.strictEqual(collector.getState().detail, 'tick 1', 'detail remains available');
  console.log('✓ 采集防重入/状态去重 OK');

  let initialNotifications = 0;
  class OfflineCollector extends BaseCollector {
    async collect() {
      return { state: 'offline', detail: 'not running' };
    }
  }
  const offline = new OfflineCollector('offline', 'Offline', {});
  offline.onStateChange(() => initialNotifications++);
  await offline._tick();
  assert.strictEqual(initialNotifications, 1, 'initial offline state must be published');
  console.log('✓ 初始状态发布 OK');
}

function testOpenClawGatewayActivity() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ai-keyboard-light-'));
  const logPath = path.join(dir, 'gateway.log');
  const now = Date.now();
  const iso = (ms) => new Date(ms).toISOString();
  const start = '[provider-transport-fetch] [model-fetch] start provider=OpenAI';
  const response = '[provider-transport-fetch] [model-fetch] response provider=OpenAI status=200';
  const collector = new OpenClawCollector({});

  fs.writeFileSync(logPath, `${iso(now - 5000)} ${start}\n`);
  assert.deepStrictEqual(collector._gatewayActivity(logPath, now), {
    working: true,
    done: false,
    error: false,
  }, 'pending model call');

  fs.writeFileSync(logPath, `${iso(now - 5000)} ${start}\n${iso(now - 1000)} ${response}\n`);
  assert.deepStrictEqual(collector._gatewayActivity(logPath, now), {
    working: false,
    done: true,
    error: false,
  }, 'completed model call');
  fs.rmSync(dir, { recursive: true, force: true });
  console.log('✓ OpenClaw 文本日志活动判定 OK');
}

function testCodexApprovalState() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ai-keyboard-light-'));
  const file = path.join(dir, 'rollout-test.jsonl');
  const now = Date.now();
  const call = {
    timestamp: new Date(now).toISOString(),
    type: 'response_item',
    payload: {
      type: 'function_call',
      call_id: 'call-approval',
      arguments: JSON.stringify({
        cmd: 'printf selection-probe > /Applications/codex-approval-probe.txt',
        sandbox_permissions: 'require_escalated',
        justification: 'approval test',
      }),
    },
  };
  fs.writeFileSync(file, JSON.stringify(call) + '\n');
  // 模拟需要人工确认的授权模式（approval_policy 非 never）
  const collector = new CodexCollector({});
  collector._apCache = 'on-request';
  assert.deepStrictEqual(
    collector.evaluateRollout(file, fs.statSync(file).mtimeMs, now),
    { state: 'waiting', detail: 'approval required' },
    'unmatched escalated function call must be waiting in manual-approval mode'
  );

  fs.appendFileSync(file, JSON.stringify({
    timestamp: new Date(now + 1).toISOString(),
    type: 'response_item',
    payload: {
      type: 'function_call_output',
      call_id: 'call-approval',
      content: 'aborted by user',
    },
  }) + '\n');
  assert.notStrictEqual(
    collector.evaluateRollout(file, fs.statSync(file).mtimeMs, now + 1).state,
    'waiting',
    'matched function call output must clear waiting'
  );

  // 完全授权（approval_policy=never）：escalated call 不判黄，自动批准后继续工作
  const autoCol = new CodexCollector({});
  autoCol._apCache = 'never';
  const st = autoCol.evaluateRollout(file, fs.statSync(file).mtimeMs, now + 1);
  assert.notStrictEqual(st.state, 'waiting', 'full-auto mode must not report waiting');
  assert.ok(['working', 'idle', 'done', 'error'].includes(st.state), 'full-auto falls to normal states');

  // 未配对 escalated call 在 never 模式下也不判黄
  const dir2 = fs.mkdtempSync(path.join(os.tmpdir(), 'ai-keyboard-light-'));
  const file2 = path.join(dir2, 'rollout-auto.jsonl');
  fs.writeFileSync(file2, JSON.stringify(call) + '\n');
  const st2 = autoCol.evaluateRollout(file2, fs.statSync(file2).mtimeMs, now);
  assert.notStrictEqual(st2.state, 'waiting', 'full-auto escalated call must not be waiting');
  fs.rmSync(dir2, { recursive: true, force: true });

  fs.rmSync(dir, { recursive: true, force: true });
  console.log('✓ Codex 审批等待判定（含完全授权模式）OK');
}

// 计划模式 / 结构化提问：未闭合的 request_user_input 必须判黄灯，
// 且即使完全授权（approval_policy=never）也要判（它会真的停下等用户）。
function testCodexRequestUserInput() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ai-keyboard-light-'));
  const file = path.join(dir, 'rollout-plan.jsonl');
  const now = Date.now();
  const ask = {
    timestamp: new Date(now).toISOString(),
    type: 'response_item',
    payload: {
      type: 'function_call',
      name: 'request_user_input',
      call_id: 'call-rui',
      arguments: JSON.stringify({
        questions: [{ header: '计划确认', id: 'plan', question: '是否按此计划执行？' }],
      }),
    },
  };
  fs.writeFileSync(file, JSON.stringify(ask) + '\n');

  // 完全授权模式下，request_user_input 仍必须判 waiting
  const autoCol = new CodexCollector({});
  autoCol._apCache = 'never';
  assert.deepStrictEqual(
    autoCol.evaluateRollout(file, fs.statSync(file).mtimeMs, now),
    { state: 'waiting', detail: 'request user input' },
    'unmatched request_user_input must be waiting even in full-auto mode'
  );

  // 手动授权模式同样判 waiting
  const manualCol = new CodexCollector({});
  manualCol._apCache = 'on-request';
  assert.strictEqual(
    manualCol.evaluateRollout(file, fs.statSync(file).mtimeMs, now).state,
    'waiting',
    'request_user_input must be waiting in manual-approval mode too'
  );

  // 用户回答后（配对 function_call_output）必须解除 waiting
  fs.appendFileSync(file, JSON.stringify({
    timestamp: new Date(now + 1000).toISOString(),
    type: 'response_item',
    payload: { type: 'function_call_output', call_id: 'call-rui', output: 'approved' },
  }) + '\n');
  const after = autoCol.evaluateRollout(file, fs.statSync(file).mtimeMs, now + 1000);
  assert.notStrictEqual(after.state, 'waiting', 'answered request_user_input must clear waiting');

  fs.rmSync(dir, { recursive: true, force: true });
  console.log('✓ Codex 计划模式/提问确认（request_user_input）黄灯判定 OK');
}

function testClaudePermissionPrompt() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ai-keyboard-light-'));
  const file = path.join(dir, '69570.json');
  const waiting = {
    pid: process.pid,
    sessionId: 'permission-test',
    status: 'waiting',
    waitingFor: 'permission prompt',
  };
  fs.writeFileSync(file, JSON.stringify(waiting));
  assert.strictEqual(isPermissionPromptActive(dir), true, 'permission prompt must be waiting');

  fs.writeFileSync(file, JSON.stringify({ ...waiting, status: 'running' }));
  assert.strictEqual(isPermissionPromptActive(dir), false, 'running session must not be waiting');
  fs.rmSync(dir, { recursive: true, force: true });
  console.log('✓ Claude 权限等待判定 OK');
}

function testCodexTokenCountHeartbeat() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ai-keyboard-light-'));
  const file = path.join(dir, 'rollout-heartbeat.jsonl');
  const now = Date.now();
  const collector = new CodexCollector({});
  collector._apCache = 'never';
  // 场景 A：30s 内只有 token_count 心跳写入（ChatGPT Desktop 任务结束后的周期心跳）
  fs.writeFileSync(file, JSON.stringify({
    timestamp: new Date(now - 1000).toISOString(),
    type: 'event_msg',
    payload: { type: 'token_count', info: { total_tokens: 12345 } },
  }) + '\n');
  const st = collector.evaluateRollout(file, fs.statSync(file).mtimeMs, now);
  assert.notStrictEqual(st.state, 'working', 'token_count heartbeat must not be working');
  assert.strictEqual(st.state, 'idle', 'token_count heartbeat should fall through to idle');

  // 场景 B：30s 内出现真实事件（function_call_output）→ working
  fs.appendFileSync(file, JSON.stringify({
    timestamp: new Date(now).toISOString(),
    type: 'response_item',
    payload: { type: 'function_call_output', call_id: 'c1', content: 'ok' },
  }) + '\n');
  const st2 = collector.evaluateRollout(file, fs.statSync(file).mtimeMs, now);
  assert.strictEqual(st2.state, 'working', 'real event within 30s must be working');
  fs.rmSync(dir, { recursive: true, force: true });
  console.log('✓ Codex token_count 心跳不误判蓝 OK');
}

testFrame();
testAggregation();
testOpenClawGatewayActivity();
testCodexApprovalState();
testCodexRequestUserInput();
testCodexTokenCountHeartbeat();
testClaudePermissionPrompt();
testCollectorTickGuards().then(() => {
  console.log('\n全部单元测试通过 ✓');
}).catch((err) => {
  console.error(err);
  process.exitCode = 1;
});
