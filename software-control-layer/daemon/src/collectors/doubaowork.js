// 豆包工作（桌面应用）— completion 闸门 + chat IndexedDB 落库停写精确检测回复完成
// v1.16 重构（2026-09-07，对照实验实测标定）：
//   旧方案 v1.15：nettop 字节流安静判定（连续 6s 无大流量 = 完成）。
//     问题：豆包空闲时也有 ~1.3-2.9KB/s 心跳流量，且生成中停顿（思考/工具/服务端排队）会误判完成；
//     实测对照（03:35:49-54 一次完整回复）：完成判定延迟约 6-9s。
//   新方案原理：
//     · chat/completion 请求 = 「发送」信号（开闸门 → 进入 working 蓝灯）
//     · 豆包把消息/回复流式落库到 Chrome IndexedDB（chrome_doubaowork-chat_0.indexeddb.leveldb）
//     · 实测：回复生成期间该目录持续写入（含 message/finish 等记录）；
//       回复完成后立即停写（103s 实测零写入）——比 nettop 干净，无心跳流量干扰
//     · 判定：leveldb 连续 LEVELDB_QUIET_MS 无任何文件变化 = 回复完成（转 done 绿）
//   双保险：
//     1) completion 闸门：只在真正发送后才监控，打字/切窗/后台同步一律忽略
//     2) 降级：leveldb 目录不可读时回退 v1.15 的 nettop 安静判定
//   兜底：
//     · MIN_WORK_MS：completion 后至少蓝 4s（防极短回复秒跳绿）
//     · MAX_WORK_MS：蓝灯最长 180s（监控异常时强制转 done，防永久卡蓝）
'use strict';

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const { BaseCollector } = require('./base');
const { APP_STATE } = require('../state');
const { config_expandHome } = require('./path');
const { isProcessRunning, nowMs } = require('./util');

// —— 回复完成检测参数（2026-09-07 对照实验标定，对应 2s 轮询窗口）——
const ACTIVE_BYTES = 6 * 1024;    // 2s 内入站增量 ≥6KB 视为 AI 流式活跃（杂项~1-3KB，AI流>6KB）
const QUIET_MS = 6 * 1000;        // 【降级用】nettop 连续 6s 无大流量 → 判定回复完成
const LEVELDB_QUIET_MS = 3 * 1000; // 【主信号】chat leveldb 连续 3s 无写入 → 回复完成
const MIN_WORK_MS = 4 * 1000;     // completion 后至少蓝 4s（防极短回复秒跳）
const MAX_WORK_MS = 180 * 1000;   // 蓝灯最长 180s 兜底（防监控失效卡死）
// —— 终端授权黄灯（实验·默认关闭，待真实弹窗标定，详见 docs/豆包终端授权黄灯-标定指引.md）——
// 背景：豆包"终端命令授权弹窗"是渲染进程内存态，零侵入下无确定性入口（native 命令在点「允许」
//   之后才下发、chat IndexedDB 只在工具完成时落盘 status=4、本地端口是 Chromium 私有口）。
//   故黄灯只能做「组合启发式」：working 中 leveldb 静止超过本阈值 + 无网络流式 + 终端工具请求
//   在途（_terminalToolPending 锚点）→ 判「疑似等待授权」亮橙。
// 双保险防误报：① config.waiting_hint_enabled 默认 false；② _terminalToolPending() 未标定前
//   恒返回 false，即使手动打开开关也不会亮黄。等现场抓到真实弹窗、标定锚点后再实现并默认开启。
const WAITING_HINT_IDLE_MS = 12 * 1000; // working 中 leveldb 连续静止 12s 才进入疑似等待判定（晚于完成判定的 3s）
const DONE_HOLD_MS = 60 * 1000;   // done 保持 60s（匹配固件 done 动画：绿闪30s+绿常亮30s）
const ERROR_HOLD_MS = 30 * 1000;   // 红灯保持 30s 自灭（匹配固件 error 动画：快闪 30s）
const ERROR_COOLDOWN_MS = 300 * 1000; // 红灯节流：同类错误距上次展示 ≥5min 才重新亮（防循环错误持续红闪）
const CHAT_LDB_DIR = '~/Library/Application Support/DoubaoWork/Default/IndexedDB/chrome_doubaowork-chat_0.indexeddb.leveldb';
// 判红纪律（2026-09-07 实测定标）：
//   1) 只认 saman 日志 ERROR 级 + [ai.userAgentWorkspace] 明确的任务操作失败。
//      真实格式：...[时间:ERROR:aha/.../ai_skill_common.cc(470)] [ai.userAgentWorkspace] operator(): createFile failed... err=XXX
//   2) 排除 FILE_ALREADY_EXISTS：实测豆包 Agent 每次任务启动都尝试创建已存在的工作文件
//      （不覆盖），err=FILE_ALREADY_EXISTS 且循环重试（2026-09-07 凌晨起 210 次，每几分钟一批 ×6）。
//      这是豆包内部无害重复错误，不是任务失败，判红会造成"任务初期红闪"误导。
//   3) 要求 err= 非空且有具体值（信息不足不判红，防噪声）。
//   4) 不认 error_message=/error_code= 等字段（网络/请求噪声，误报率高）。
const ERROR_RE = /:ERROR:[^\]]*\]\s*\[ai\.userAgentWorkspace\]\s*[^\n]*failed[^\n]*err=(?!FILE_ALREADY_EXISTS)[A-Za-z_0-9 -]{1,60}/i;

// 汇总所有豆包 Browser/Renderer 进程的「累计入站字节」。
// 用进程名前缀全量匹配而非单个 PID：豆包是多进程架构且 PID 会随重启变化，
// 汇总全部 DoubaoWork Brow* 即可自适应，无需维护 PID（实测单次 ~0.1s）。
function readDoubaoBytesIn() {
  try {
    const out = execFileSync(
      '/bin/sh',
      [
        '-c',
        // -n disables DNS resolution and -P asks for per-process totals. Without
        // them nettop can wait about 30s per sample and block the daemon event loop.
        `nettop -n -L 1 -x -P -J bytes_in 2>/dev/null | grep -i "DoubaoWork Brow" | awk -F, '{s+=$2} END{print s+0}'`,
      ],
      { stdio: ['ignore', 'pipe', 'ignore'], timeout: 3000 }
    )
      .toString()
      .trim();
    const n = Number(out);
    return Number.isFinite(n) ? n : -1;
  } catch {
    return -1;
  }
}

class DoubaoWorkCollector extends BaseCollector {
  constructor(config) {
    super('doubaowork', '豆包工作', config);
    this._phase = 'idle'; // idle | working | done
    this._workStart = 0; // working 起点（首次 completion）
    this._lastActive = 0; // 最后一次活跃时刻（大流量 / 新 completion）
    this._doneAt = 0; // 进入 done 的时刻
    this._prevBytes = -1; // 上次采样的累计入站字节
    this._logPath = null;
    this._lastOffset = 0; // 日志增量读取偏移
    this._lvlSig = null; // chat leveldb 上次指纹
    this._lastLvlActive = 0; // 最近一次 leveldb 写入时刻
    this._lvlUsable = false; // leveldb 是否可读（不可读时降级 nettop 判定）
    this._errAt = 0; // 最近一次任务失败（saman [ERROR: 级）时刻
    this._lastErrShown = 0; // 上次红灯展示时刻（节流用：同类错误 ≥5min 才重新亮）
    this._pendingCompletion = false; // 红灯期间用户发了新消息（红灯结束后立即进入 working）
  }

  // chat IndexedDB 目录指纹：任意文件 (name:size:mtime) 变化 = 有新写入
  _leveldbSig() {
    try {
      const dir = config_expandHome(CHAT_LDB_DIR);
      const files = fs.readdirSync(dir);
      let sig = '';
      for (const f of files) {
        if (!/\.(log|ldb)$/.test(f)) continue;
        try {
          const st = fs.statSync(path.join(dir, f));
          sig += `${f}:${st.size}:${st.mtimeMs};`;
        } catch { /* 文件瞬移（轮转）忽略 */ }
      }
      return sig || null;
    } catch {
      return null;
    }
  }

  // 找当天的 saman_YYYY.MMDD.0.log；不存在则用最新一个 saman_*.log
  _findLog(logDir) {
    if (!logDir) return null;
    let files = [];
    try {
      files = fs.readdirSync(logDir).filter(f => /^saman_\d{4}\.\d{4}\.0\.log$/.test(f));
    } catch {
      return null;
    }
    if (files.length === 0) return null;
    const now = new Date();
    const ymd = `${now.getFullYear()}.${String(now.getMonth() + 1).padStart(2, '0')}${String(now.getDate()).padStart(2, '0')}`;
    const today = path.join(logDir, `saman_${ymd}.0.log`);
    if (fs.existsSync(today)) return today;
    files.sort();
    return path.join(logDir, files[files.length - 1]);
  }

  // 增量读取日志新内容，返回 { newCompletion, newError }
  //  - newCompletion：是否包含 chat/completion（发送信号）
  //  - newError：是否包含 [ERROR: 级任务操作失败（红灯信号）
  _scanLogChunk(logPath) {
    try {
      const st = fs.statSync(logPath);
      // 文件切换/轮转：跳过历史，只从当前末尾开始检测后续新增
      if (logPath !== this._logPath || st.size < this._lastOffset) {
        this._lastOffset = st.size;
        this._logPath = logPath;
        return { newCompletion: false, newError: false };
      }
      if (st.size <= this._lastOffset) return { newCompletion: false, newError: false };
      const fd = fs.openSync(logPath, 'r');
      const buf = Buffer.alloc(st.size - this._lastOffset);
      fs.readSync(fd, buf, 0, buf.length, this._lastOffset);
      fs.closeSync(fd);
      this._lastOffset = st.size;
      const txt = buf.toString('utf8');
      return {
        newCompletion: txt.includes('chat/completion'),
        newError: ERROR_RE.test(txt),
      };
    } catch {
      return { newCompletion: false, newError: false };
    }
  }

  _resetPhase() {
    this._phase = 'idle';
    this._workStart = 0;
    this._lastActive = 0;
    this._doneAt = 0;
    this._errAt = 0;
    this._lastErrShown = 0;
    this._pendingCompletion = false;
    // _prevBytes 保留，避免跨阶段把历史累计误算成增量
  }

  _resetLeveldb() {
    this._lvlSig = null;
    this._lastLvlActive = 0;
    this._lvlUsable = false;
  }

  // 【实验锚点·待标定】是否存在「终端工具请求已发起、但结果块尚未落盘」（即疑似卡在授权弹窗）。
  // 现状（2026-09-08 逆向结论）：chat IndexedDB 里终端工具块只在完成时一次性落盘
  //   （status=4 / is_finish=true），待授权中间态只在渲染进程内存、不落盘；本地端口是 Chromium
  //   私有口、native 命令在点「允许」后才下发。因此零侵入下当前无法稳定判定，恒返回 false。
  // 标定方法：在非全自动会话豆包自然弹出终端授权时，先别点，运行
  //   scripts/doubao-auth-capture.py 抓挂起态，确认是否存在「请求已落盘、结果未回」的可读特征；
  //   若找到则在此实现该特征检测（返回 true/false），并把 config.waiting_hint_enabled 默认打开。
  _terminalToolPending() {
    return false;
  }

  async collect() {
    const cfg = this.config || {};
    const logDir = config_expandHome(cfg.log_dir);
    const now = nowMs();

    // 进程检测：豆包工作
    const running = isProcessRunning('DoubaoWork') || isProcessRunning('DoubaoWork.app');
    if (!running) {
      this._resetPhase();
      this._resetLeveldb();
      this._prevBytes = -1;
      this._logPath = null;
      this._lastOffset = 0;
      return { state: APP_STATE.OFFLINE, detail: 'no process' };
    }

    // 1) 日志增量：completion（发送信号）+ ERROR（红灯信号）
    let newCompletion = false;
    let newError = false;
    const logPath = this._findLog(logDir);
    if (logPath) {
      const chunk = this._scanLogChunk(logPath);
      newCompletion = chunk.newCompletion;
      newError = chunk.newError;
    }
    if (newCompletion) {
      this._lastActive = now;
      this._pendingCompletion = true; // 红灯期间发消息：红灯结束后立即进入 working
    }
    // 红灯节流：距上次红灯展示 ≥5min 的新错误才更新 _errAt（重新亮）；
    // 冷却期内的重复错误（如豆包循环重试）不再触发红灯。
    if (newError && now - this._lastErrShown >= ERROR_COOLDOWN_MS) this._errAt = now;

    // 0) 错误优先（红 > 橙 > 绿 > 蓝 > 灭）：30s 内有明确任务失败 → 红灯
    if (this._errAt > 0) {
      if (now - this._errAt <= ERROR_HOLD_MS) {
        this._lastErrShown = now; // 记录红灯展示时刻（节流基准）
        if (this._phase !== 'working') this._phase = 'idle'; // 红灯期间清掉旧 phase
        return { state: APP_STATE.ERROR, detail: 'saman task error' };
      }
      this._errAt = 0; // 错误保持窗口过期，自灭
    }
    // 红灯期间用户发了新消息 → 红灯结束后立即进入 working
    if (this._pendingCompletion) {
      this._pendingCompletion = false;
      this._phase = 'working';
      this._workStart = now;
      this._lastActive = now;
      return { state: APP_STATE.WORKING, detail: 'completion after error' };
    }

    // 2) 网络字节增量（相对上一个 2s 轮询周期）
    const totalBytes = readDoubaoBytesIn();
    let delta = 0;
    if (totalBytes >= 0 && this._prevBytes >= 0 && totalBytes >= this._prevBytes) {
      delta = totalBytes - this._prevBytes;
    }
    if (totalBytes >= 0) this._prevBytes = totalBytes;
    const burst = delta >= ACTIVE_BYTES;

    // 2.5) chat IndexedDB 落库信号（主信号）：目录指纹变化 = 回复生成中
    const lvlSig = this._leveldbSig();
    let lvlActive = false;
    if (lvlSig !== null) {
      if (this._lvlSig === null) {
        this._lvlSig = lvlSig; // 首次基线，不判活跃
        this._lvlUsable = true;
      } else if (lvlSig !== this._lvlSig) {
        this._lvlSig = lvlSig;
        lvlActive = true;
        this._lastLvlActive = now;
        this._lvlUsable = true;
      }
    }

    // 3) 状态机
    if (this._phase === 'idle') {
      if (newCompletion) {
        // 发送 → 开闸门，进入 working（蓝灯起点 = 真实发送时刻）
        this._phase = 'working';
        this._workStart = now;
        this._lastActive = now;
        return { state: APP_STATE.WORKING, detail: 'completion gate open' };
      }
      // 打字/切窗/后台流量：无 completion 闸门，一律不亮
      return { state: APP_STATE.IDLE, detail: 'idle' };
    }

    if (this._phase === 'working') {
      // 同一条消息会产生 2-3 个 completion 请求；SSE 数据/落库写入也刷新活跃时刻
      if (newCompletion) this._lastActive = now;
      if (burst) this._lastActive = now;
      if (lvlActive) this._lastActive = now;

      const worked = now - this._workStart;
      const lvlQuiet = (this._lvlUsable && this._lastLvlActive > 0) ? now - this._lastLvlActive : -1;

      // 【实验·默认关闭】终端授权黄灯（组合启发式）。
      // 仅当 config.waiting_hint_enabled===true 才进入；且 _terminalToolPending() 未标定前恒 false，
      // 所以在真实弹窗标定锚点之前，即使手动打开开关也不会亮黄（双保险，杜绝误报）。
      if (cfg.waiting_hint_enabled === true && worked >= MIN_WORK_MS) {
        const stalled = lvlQuiet >= 0 ? lvlQuiet : (now - this._lastActive);
        if (stalled >= WAITING_HINT_IDLE_MS && !burst && this._terminalToolPending()) {
          return {
            state: APP_STATE.WAITING,
            detail: `terminal-auth hint, idle ${Math.round(stalled / 1000)}s`,
          };
        }
      }

      let done = false;
      let why = '';
      if (this._lvlUsable && this._lastLvlActive > 0) {
        // 主信号：leveldb 连续无写入达到去抖窗口 = 回复完成（比 nettop 干净，无心跳干扰）
        if (worked >= MIN_WORK_MS && lvlQuiet >= LEVELDB_QUIET_MS) {
          done = true;
          why = `ldb idle ${Math.round(worked / 1000)}s`;
        }
      } else {
        // 降级：leveldb 不可用或尚未见写入 → nettop 安静判定
        const quiet = now - this._lastActive;
        if (worked >= MIN_WORK_MS && quiet >= QUIET_MS) {
          done = true;
          why = `nettop quiet ${Math.round(worked / 1000)}s`;
        }
      }

      if (done) {
        this._phase = 'done';
        this._doneAt = now;
        return {
          state: APP_STATE.DONE,
          detail: `${why}, worked ${Math.round(worked / 1000)}s`,
        };
      }
      // 兜底：超过最大工作窗口仍在"接收"，强制转 done（防监控异常卡蓝）
      if (worked >= MAX_WORK_MS) {
        this._phase = 'done';
        this._doneAt = now;
        return { state: APP_STATE.DONE, detail: 'max-work-window fallback' };
      }
      return { state: APP_STATE.WORKING, detail: 'streaming' };
    }

    // phase === 'done'
    if (newCompletion) {
      // done 保持期内用户又发了新消息 → 立刻回到 working
      this._phase = 'working';
      this._workStart = now;
      this._lastActive = now;
      return { state: APP_STATE.WORKING, detail: 'new completion' };
    }
    if (now - this._doneAt <= DONE_HOLD_MS) {
      return { state: APP_STATE.DONE, detail: 'done hold' };
    }
    this._resetPhase();
    return { state: APP_STATE.IDLE, detail: 'done expired' };
  }
}

module.exports = { DoubaoWorkCollector };
