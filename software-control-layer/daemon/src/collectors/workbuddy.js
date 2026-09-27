// WorkBuddy 采集器 — 无侵入文件监控（不占灯位，仅接入状态监控）
// 来源：~/.openclaw/workbuddy-publish-* 项目目录树（WorkBuddy Skill 发布包工作流产物）
// 逻辑：WorkBuddy 无独立进程，任务运行在 OpenClaw（gateway）内；
//       · OpenClaw 未运行 → offline（WorkBuddy 不可能活动）
//       · 项目目录不存在 → offline
//       · 目录树最近 60s 内有文件写入 → working（发布任务正在产出）
//       · 否则 → idle
// 注：本采集器状态仅供前端/API 展示，不参与灯位映射（lights 配置不含 workbuddy）。
'use strict';

const fs = require('fs');
const path = require('path');
const { BaseCollector } = require('./base');
const { APP_STATE } = require('../state');
const { config_expandHome } = require('./path');
const { isProcessRunningPattern, nowMs } = require('./util');

const WORKING_WINDOW_MS = 60 * 1000; // 60s 内有写入 → 工作中
const MAX_DEPTH = 6;                 // 扫描深度
const SCAN_LIMIT = 2000;             // 单轮最多 stat 的文件数（防大目录拖慢轮询）

// 递归统计目录树内最新文件 mtime（超过 SCAN_LIMIT 即截断，够用即可）
function newestMtime(dir, depth, limitState) {
  if (depth > MAX_DEPTH || limitState.count >= SCAN_LIMIT) return 0;
  let newest = 0;
  let entries;
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return 0;
  }
  for (const e of entries) {
    if (limitState.count >= SCAN_LIMIT) break;
    const full = path.join(dir, e.name);
    if (e.isDirectory()) {
      const t = newestMtime(full, depth + 1, limitState);
      if (t > newest) newest = t;
    } else {
      limitState.count++;
      try {
        const st = fs.statSync(full);
        if (st.mtimeMs > newest) newest = st.mtimeMs;
      } catch {
        /* 权限/瞬时消失，忽略 */
      }
    }
  }
  return newest;
}

class WorkBuddyCollector extends BaseCollector {
  constructor(config) {
    super('workbuddy', 'WorkBuddy', config);
  }

  async collect() {
    const cfg = this.config || {};
    const baseDir = config_expandHome(cfg.work_dir || '~/.openclaw');
    const now = nowMs();

    // OpenClaw 门控：WorkBuddy 任务跑在 OpenClaw 里，宿主不在则必不可能活动。
    // 进程名是 node，用命令行模糊匹配（pgrep -f openclaw）
    const openclawUp = isProcessRunningPattern('openclaw');
    if (!openclawUp) {
      return { state: APP_STATE.OFFLINE, detail: 'openclaw not running' };
    }

    // 扫描 workbuddy-publish-* 项目目录
    let newest = 0;
    try {
      const entries = fs.readdirSync(baseDir, { withFileTypes: true });
      for (const e of entries) {
        if (!e.isDirectory()) continue;
        if (!/^workbuddy-publish(-|$)/.test(e.name)) continue;
        const t = newestMtime(path.join(baseDir, e.name), 0, { count: 0 });
        if (t > newest) newest = t;
      }
    } catch {
      return { state: APP_STATE.OFFLINE, detail: 'workbuddy dir missing' };
    }

    if (newest === 0) {
      return { state: APP_STATE.OFFLINE, detail: 'no workbuddy project' };
    }

    if (now - newest <= WORKING_WINDOW_MS) {
      return { state: APP_STATE.WORKING, detail: 'publish activity' };
    }
    return { state: APP_STATE.IDLE, detail: 'idle' };
  }
}

module.exports = { WorkBuddyCollector };
