// 采集器真实验证：用本机真实状态文件各采集一次并打印
'use strict';

const { loadConfig } = require('./src/config');
const { CodexCollector } = require('./src/collectors/codex');
const { ClaudeCollector } = require('./src/collectors/claude');
const { OpenClawCollector } = require('./src/collectors/openclaw');
const { DoubaoWorkCollector } = require('./src/collectors/doubaowork');
const { DeepseekCollector } = require('./src/collectors/deepseek');
const { PiCollector } = require('./src/collectors/pi');

async function main() {
  const config = loadConfig();
  const builders = {
    codex: CodexCollector,
    claude: ClaudeCollector,
    openclaw: OpenClawCollector,
    doubaowork: DoubaoWorkCollector,
    deepseek: DeepseekCollector,
    pi: PiCollector,
  };
  for (const [appId, Ctor] of Object.entries(builders)) {
    const ccfg = config.collectors[appId];
    const c = new Ctor({ ...ccfg });
    const r = await c.collect();
    console.log(`${appId.padEnd(12)} → ${String(r.state).padEnd(8)} | ${r.detail}`);
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
