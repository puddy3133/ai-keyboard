// CodeX CLI / Desktop / TUI — 无 hooks 文件监控
// 来源：~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl + .codex-global-state.json
// v1.11：多会话聚合——扫描最近活跃的 rollout，各自评估状态，按 红>黄>绿>蓝>灭 取最高优先级。
'use strict';

const fs = require('fs');
const { BaseCollector } = require('./base');
const { APP_STATE } = require('../state');
const { config_expandHome } = require('./path');
const {
  isProcessMatching,
  recentFiles,
  tailLines,
  nowMs,
} = require('./util');

const DONE_HOLD_MS = 60 * 1000;  // 匹配固件 done 动画(绿闪30s+常亮30s)
// 只聚合最近活跃的会话（避免扫描全部历史 rollout）
const ACTIVE_WINDOW_MS = 15 * 60 * 1000;
const MAX_SESSIONS = 8;
// 挂起档窗口：waiting（计划模式批准/结构化提问/权限升级）本质是"会话文件静止"，
// 静止时长 = 用户思考时长，实测可达 73 分钟，远超 15min 活跃窗口。
// 额外用该长窗口扫描，只捞"评估为 waiting"的静止会话，避免用户思考期间黄灯被活跃窗误杀。
const PENDING_WINDOW_MS = 6 * 60 * 60 * 1000;

class CodexCollector extends BaseCollector {
  constructor(config) {
    super(config.appId || 'codex', config.appName || 'CodeX', config);
    // 窗口跟随缓存与冷却：避免每轮 2s 都调系统命令/osascript，防止权限问题拖慢采集器轮询
    this._focusCache = null;        // { at, result }
    this._osascriptCoolUntil = 0;   // osascript 失败后的冷却时间戳
    this._apCache = null;           // approval_policy 缓存
    this._apCacheAt = 0;
  }

  // 读取 Codex 引擎的 approval_policy（~/.codex/config.toml，CLI 与 ChatGPT Desktop 共用）。
  // approval_policy="never" = 完全授权：权限请求仍会产生（escalated function_call），
  // 但会被自动批准继续执行，不会真正停下等人工确认 → 该模式下不应判黄灯。
  // 读不到配置时保守返回 null（按需要人工确认处理，可判黄）。
  _approvalPolicy() {
    const now = nowMs();
    // _apCacheAt===0 表示手动注入/尚未计时（测试友好），直接信任非 null 缓存值
    if (this._apCache !== null && (this._apCacheAt === 0 || now - this._apCacheAt < 5000)) {
      return this._apCache;
    }
    let policy = null;
    try {
      const txt = fs.readFileSync(config_expandHome('~/.codex/config.toml'), 'utf8');
      const m = txt.match(/approval_policy\s*=\s*"([^"]+)"/);
      if (m) policy = m[1];
    } catch {
      // 读取失败按保守处理
    }
    this._apCache = policy;
    this._apCacheAt = now;
    return policy;
  }

  // 完全授权判定：never = 永不询问、自动批准
  _autoApproval() {
    return this._approvalPolicy() === 'never';
  }

  // 读取会话文件的来源标识（session_meta.originator）：codex-tui=CLI / Codex Desktop=ChatGPT桌面 等
  readOriginator(filePath) {
    try {
      const first = fs.readFileSync(filePath, 'utf8').split('\n').find(Boolean);
      if (!first) return '';
      return (JSON.parse(first).payload || {}).originator || '';
    } catch {
      return '';
    }
  }

  // 评估单个 rollout 会话的状态
  evaluateRollout(filePath, mtimeMs, now) {
    const ageMs = now - mtimeMs;
    const lines = tailLines(filePath);

    // Codex records an escalated tool call before the approval result arrives.
    // An unmatched approval request is waiting, not ordinary model work.
    // value = { args, name }：name 用于识别 request_user_input（计划模式/结构化提问确认）。
    const pendingApprovals = new Map();
    for (const row of lines) {
      const payload = row?.payload || {};
      if (payload.type === 'function_call') {
        let args = payload.arguments;
        if (typeof args === 'string') {
          try { args = JSON.parse(args); } catch { args = null; }
        }
        const callId = payload.call_id || payload.id;
        // 只要有 callId 就登记（request_user_input 判定只依赖 name，不应因 args 解析失败漏掉）
        if (callId) pendingApprovals.set(callId, { args, name: payload.name });
      } else if (payload.type === 'function_call_output' && payload.call_id) {
        pendingApprovals.delete(payload.call_id);
      }
    }

    // 跳过中性事件（token_count/usage 等），取最后一条"有意义"的记录
    let last = null;
    for (let i = lines.length - 1; i >= 0; i--) {
      const row = lines[i];
      const type = row?.type;
      const ptype = row?.payload?.type;
      if (type === 'event_msg' && ['token_count', 'usage', 'token_counts'].includes(ptype)) {
        continue;
      }
      if (type === 'event_msg' && !ptype) continue;
      last = row;
      break;
    }

    const type = last ? last.type : '';
    const pl = (last && last.payload) || {};

    // 完成（task_complete 优先于 working；以文件最后写入时刻为基准 30s hold，不依赖状态变量）
    if (type === 'event_msg' && pl.type === 'task_complete') {
      // CodeX 模型/上游报错时会写 task_complete + payload.error → 必须判红色 ERROR 而非 done
      if (pl.error) {
        return {
          state: APP_STATE.ERROR,
          detail: typeof pl.error === 'string' ? pl.error : (pl.error.message || 'model error'),
        };
      }
      if (now - mtimeMs <= DONE_HOLD_MS) {
        return { state: APP_STATE.DONE, detail: 'done' };
      }
      return { state: APP_STATE.IDLE, detail: 'done expired' };
    }

    // 错误（仅明确错误字段）：pl.error 是任务级失败信号；工具输出内容含 "error"/"failed"
    // 字样不算任务失败（AI 读取日志/测试输出时经常出现这些词，直接判红会造成误报）
    if (pl.error) {
      return {
        state: APP_STATE.ERROR,
        detail: typeof pl.error === 'string' ? pl.error : (pl.error.message || 'model error'),
      };
    }

    // 完全授权（approval_policy=never）模式下权限请求会被自动批准并继续，
    // 不会真正停下等人工确认，因此不判黄灯（否则 full-auto 长任务会黄灯闪动）。
    // 待确认①：request_user_input（计划模式 plan / 结构化提问）。
    // 实测（2026-09-08）：模型需要用户选择/确认时写 function_call name="request_user_input"
    // （arguments.questions 为结构化选项），用户回答后才有配对 function_call_output；
    // 一次确认实测停留 10 分钟。它与权限升级不同——即使 approval_policy=never（完全授权），
    // request_user_input 仍会真的停下等用户，因此必须独立于 _autoApproval() 判定。
    // CLI（codex-tui）与 ChatGPT Desktop 共用同一 rollout 格式，一并生效。
    for (const pend of pendingApprovals.values()) {
      if (pend.name === 'request_user_input') {
        return { state: APP_STATE.WAITING, detail: 'request user input' };
      }
    }

    // 待确认②：升级权限请求。完全授权（approval_policy=never）下权限请求会被自动批准并继续，
    // 不会真正停下等人工确认，因此不判黄灯（否则 full-auto 长任务会黄灯闪动）。
    if (!this._autoApproval()) {
      for (const pend of pendingApprovals.values()) {
        const args = pend.args;
        if (
          args && typeof args === 'object' && (
            args.sandbox_permissions === 'require_escalated' ||
            typeof args.justification === 'string'
          )
        ) {
          return { state: APP_STATE.WAITING, detail: 'approval required' };
        }
      }
    }

    // 工作中：180s 内有"真实事件"写入才算工作。
    // 实测（2026-09-07）：ChatGPT Desktop / CodeX CLI 长任务节奏为"执行一批工具 → AI 思考 →
    // 再执行"。思考间隙实测可达 130s（09:44:38→09:46:48 零写入，长上下文推理）。
    // 因此窗口从 30s→90s→180s 逐步放宽，避免任务中灭灯。
    // 真正完成由 task_complete 硬信号判定（上方分支），不依赖此窗口，放宽不会拖延完成检测。
    // 同时排除 token_count/usage 等中性心跳：它们不产生任何任务活动。
    if (ageMs < 180 * 1000) {
      const realRecent = lines.some((row) => {
        if (!row || !row.timestamp) return false;
        if (now - Date.parse(row.timestamp) > 180 * 1000) return false;
        const ptype = row.payload && row.payload.type;
        if (row.type === 'event_msg' && (!ptype || ['token_count', 'usage', 'token_counts'].includes(ptype))) {
          return false;
        }
        return true;
      });
      if (realRecent) return { state: APP_STATE.WORKING, detail: 'active' };
    }

    if (!last) {
      return { state: APP_STATE.IDLE, detail: 'no meaningful event' };
    }

    // 推理/调工具 → 工作中
    if (
      type === 'event_msg' && ['agent_message', 'reasoning', 'response'].includes(pl.type) ||
      type === 'response_item' && pl.type === 'function_call'
    ) {
      return { state: APP_STATE.WORKING, detail: `active (${pl.type})` };
    }

    // 待命
    return { state: APP_STATE.IDLE, detail: 'idle' };
  }

  async collect() {
    const cfg = this.config || {};
    const sessionsDir = config_expandHome(cfg.sessions_path);
    const globalPath = config_expandHome(cfg.global_state_path);
    // 精确进程匹配（config.procInclude/procExclude 区分 CodeX CLI 与 ChatGPT Desktop）
    const running = isProcessMatching({
      include: cfg.procInclude || ['codex'],
      exclude: cfg.procExclude || [],
    });
    // 进程退出 → 尽快灭灯（连续 2 轮 ~4s 确认，避免进程瞬时抖动误灭）
    this._downStreak = running ? 0 : (this._downStreak || 0) + 1;
    if (this._downStreak >= 2) {
      return { state: APP_STATE.OFFLINE, detail: 'proc exited' };
    }

    // unread 标记（CodeX Desktop）——应用级，最高优先级（黄）
    let unread = false;
    try {
      const g = JSON.parse(fs.readFileSync(globalPath, 'utf8'));
      const map =
        g?.['electron-persisted-atom-state']?.['unread-thread-ids-by-host-v1'];
      if (map && Array.isArray(map.local) && map.local.length > 0) {
        unread = true;
      }
    } catch {
      /* 全局状态不可读则忽略 unread */
    }

    const now = nowMs();
    const files = recentFiles(sessionsDir, {
      ext: '.jsonl',
      maxDepth: 5,
      windowMs: ACTIVE_WINDOW_MS,
      // CodeX CLI and ChatGPT Desktop share this directory. Scan both
      // sources before filtering so one source cannot starve the other.
      limit: MAX_SESSIONS * 2,
    });
    // 挂起档：用更长窗口补扫静止会话（waiting 期间文件不再写入，会落在 15min 活跃窗之外）。
    // 只 stat 不读内容、去重活跃档，开销可控。
    const activePaths = new Set(files.map((f) => f.path));
    const pendingFiles = recentFiles(sessionsDir, {
      ext: '.jsonl',
      maxDepth: 5,
      windowMs: PENDING_WINDOW_MS,
      limit: MAX_SESSIONS * 4,
    }).filter((f) => !activePaths.has(f.path));
    if (files.length === 0 && pendingFiles.length === 0) {
      return {
        state: running ? APP_STATE.IDLE : APP_STATE.OFFLINE,
        detail: 'no rollout',
      };
    }

    // 按来源过滤（originators 白名单）：CodeX CLI=codex-tui，ChatGPT Desktop=Codex Desktop，互不干扰
    const originators = Array.isArray(this.config?.originators) ? this.config.originators : null;
    const matchOrigin = (p) => !originators || !originators.length || originators.includes(this.readOriginator(p));
    const sessions = [];
    const seen = new Set();
    // ① 挂起档优先：只纳入评估为 waiting 的静止会话，且不占用 MAX_SESSIONS 活跃名额，
    //    保证"长时间等用户确认"的黄灯不被活跃会话挤占、不被 15min 窗口误杀。
    for (const f of pendingFiles) {
      if (!matchOrigin(f.path)) continue;
      const st = this.evaluateRollout(f.path, f.mtimeMs, now);
      if (st.state === APP_STATE.WAITING) {
        sessions.push({ path: f.path, state: st, mtimeMs: f.mtimeMs });
        seen.add(f.path);
      }
    }
    // ② 活跃档：最近 15min 的会话全部评估，受 MAX_SESSIONS 截断。
    for (const f of files) {
      if (seen.has(f.path)) continue;
      if (!matchOrigin(f.path)) continue;
      sessions.push({
        path: f.path,
        state: this.evaluateRollout(f.path, f.mtimeMs, now),
        mtimeMs: f.mtimeMs,
      });
      seen.add(f.path);
      if (sessions.length >= MAX_SESSIONS) break;
    }
    if (sessions.length === 0) {
      return {
        state: running ? APP_STATE.IDLE : APP_STATE.OFFLINE,
        detail: 'no matching session',
      };
    }

    // 窗口跟随：仅 CLI 采集器启用（focus_follow=false 跳过）。若 Terminal 前台窗口对应某会话视为"用户当前关注"。
    const focused = this.config?.focus_follow === false ? null : this._resolveFocusedSession();
    if (focused) {
      const hit = sessions.find((s) => s.path === focused.path);
      if (hit) {
        hit.mtimeMs = now; // 焦点会话视为最近活跃
      } else {
        // 焦点会话提升前先验证来源匹配，防止非目标来源混入
        if (!originators || !originators.length || originators.includes(this.readOriginator(focused.path))) {
          sessions.push({
            path: focused.path,
            state: this.evaluateRollout(focused.path, focused.mtimeMs, now),
            mtimeMs: now,
          });
        }
      }
    }

    // Desktop unread 应用级提醒（黄）：默认关闭（用户确认不需要），仅当显式配置
    // unread_enabled: true 时才启用。unread 是 Codex Desktop 的"未读线程"概念，
    // 且 unread-thread-ids 文件是全局共享的，CLI 采集器不应读取它，否则会把
    // ChatGPT Desktop 的未读误判到 L1（CLI 灯）→ 黄闪。
    if (unread && this.config?.unread_enabled === true) {
      const waiting = sessions.find((s) => s.state.state === APP_STATE.WAITING);
      if (waiting) return waiting.state;
      const active = [...sessions].sort((a, b) => b.mtimeMs - a.mtimeMs)[0].state;
      if (active.state === APP_STATE.ERROR) return active;
      return { state: APP_STATE.WAITING, detail: 'unread' };
    }

    return this.aggregateBehavior(sessions);
  }

  // 解析 macOS Terminal 前台窗口对应的 CodeX 焦点会话（零侵入窗口跟随）。
  // 链路：前台应用=Terminal → 前台窗口 tty → 该 tty 上的 codex 进程 → 进程启动时间匹配会话创建时间。
  // 弹性设计（避免守护进程权限问题拖慢轮询）：
  //  - 前台检测用 lsappinfo（无需 Apple Events 权限）；前台非 Terminal → 零系统命令开销
  //  - 仅前台是 Terminal 才调 osascript 拿 tty；失败进入 30s 冷却，不反复尝试
  //  - 结果缓存 2.5s 复用，避免每轮 2s 都重复解析
  _resolveFocusedSession() {
    try {
      const now = Date.now();
      // 缓存复用：2.5s 内不重复解析
      if (this._focusCache && now - this._focusCache.at < 2500) {
        return this._focusCache.result;
      }
      const { execSync } = require('child_process');
      const sh = (cmd, timeout) => {
        try {
          return execSync(cmd, { encoding: 'utf8', timeout: timeout || 2000, stdio: ['ignore', 'pipe', 'ignore'] }).trim();
        } catch {
          return '';
        }
      };
      const miss = () => { this._focusCache = { at: now, result: null }; return null; };
      // 0) 前置：必须有 codex CLI 进程才做窗口跟随
      if (!isProcessMatching({ include: this.config?.procInclude || ['codex'], exclude: this.config?.procExclude || [] })) return miss();
      // 1) 前台应用（lsappinfo，无需 Apple Events 权限）
      const frontAsn = sh('lsappinfo front', 1000);
      const nameLine = frontAsn ? sh(`lsappinfo info -only name "${frontAsn}"`, 1000) : '';
      const frontApp = (nameLine.match(/"LSDisplayName"="([^"]+)"/) || [])[1] || '';
      if (frontApp !== 'Terminal') return miss();
      // 2) osascript 失败冷却：上次失败后 30s 内不再尝试
      if (now < this._osascriptCoolUntil) return miss();
      // 3) Terminal 前台窗口 tty（osascript；需要 Apple Events 权限，带严格超时）
      const tty = sh(`perl -e 'alarm 1; exec @ARGV' osascript -e 'tell application "Terminal" to get tty of front window'`, 2000);
      if (!tty.startsWith('/dev/')) {
        this._osascriptCoolUntil = now + 30 * 1000; // 进入冷却，避免反复调用拖慢轮询
        return miss();
      }
      // 4) 该 tty 上的 codex 进程（排除 shim / daemon / extension）
      const rows = sh(`ps -eo pid,tty,lstart,command | grep -E "codex" | grep -v grep`, 2000)
        .split('\n')
        .filter((l) => l.includes(tty) && !/responses_chat_shim|daemon\.mjs|codex-to-im|chrome-extension/.test(l));
      if (rows.length === 0) return miss();
      const pid = rows[0].trim().split(/\s+/)[0];
      // 5) 进程启动时间（HH:MM:SS）
      const start = sh(`ps -o lstart= -p ${pid}`, 1000).trim();
      const sm = start.match(/(\d\d):(\d\d):(\d\d)/);
      if (!sm) return miss();
      const startKey = sm[1] + ':' + sm[2] + ':' + sm[3];
      // 6) 匹配会话：创建时间（文件名 T 时间戳，含秒）≥ 进程启动时间，取其中 mtime 最新
      const dir = config_expandHome(this.config?.sessions_path || '~/.codex/sessions');
      const files = recentFiles(dir, { ext: '.jsonl', maxDepth: 5, windowMs: 4 * 60 * 60 * 1000, limit: 30 });
      let best = null;
      for (const f of files) {
        const b = f.path.split('/').pop();
        const t = b.match(/T(\d\d)-(\d\d)-(\d\d)/);
        if (!t) continue;
        const fileKey = t[1] + ':' + t[2] + ':' + t[3];
        if (fileKey >= startKey && (!best || f.mtimeMs > best.mtimeMs)) best = f;
      }
      const result = best ? { path: best.path, mtimeMs: best.mtimeMs } : null;
      this._focusCache = { at: now, result };
      return result;
    } catch {
      return null;
    }
  }
}

module.exports = { CodexCollector };
