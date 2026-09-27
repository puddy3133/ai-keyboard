// 公共工具：进程检测、mtime、JSONL 尾部读取、sqlite 查询
'use strict';

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

// 进程是否运行（精确匹配，避免误判同名）
function isProcessRunning(procName) {
  try {
    const out = execFileSync('/bin/sh', ['-c', `pgrep -x ${JSON.stringify(procName)}`], {
      stdio: ['ignore', 'pipe', 'ignore'],
      timeout: 3000,
    }).toString();
    return out.trim().length > 0;
  } catch {
    return false;
  }
}

// 模糊进程检测（匹配命令行片段，如 "dsh web"）
function isProcessRunningPattern(pattern) {
  try {
    const out = execFileSync('/bin/sh', ['-c', `pgrep -f ${JSON.stringify(pattern)}`], {
      stdio: ['ignore', 'pipe', 'ignore'],
      timeout: 3000,
    }).toString();
    return out.trim().length > 0;
  } catch {
    return false;
  }
}

// 精确进程匹配：遍历所有进程命令行，include 任一命中 且 exclude 全部未命中才算"在线"。
// 用于区分同名/同族进程（如 CodeX CLI 的 bin/codex vs ChatGPT Desktop 的 Resources/codex，
// 以及 responses_chat_shim 命令行里的 claude 模型名），避免误把其他应用算作本应用在线。
function isProcessMatching({ include = [], exclude = [] } = {}) {
  if (!Array.isArray(include) || include.length === 0) return false;
  try {
    const out = execFileSync('/bin/ps', ['-wwaxo', 'command='], {
      stdio: ['ignore', 'pipe', 'ignore'],
      timeout: 3000,
    }).toString();
    return out.split('\n').some((line) => {
      if (!line.trim()) return false;
      if (exclude.some((p) => line.includes(p))) return false;
      return include.some((p) => line.includes(p));
    });
  } catch {
    return false;
  }
}

// 目录下（非递归或递归）最近修改文件的 mtime（ms）。返回 0 表示无文件。
function recentMtimeMs(dirPath, { recursive = false } = {}) {
  let stat = null;
  try {
    stat = fs.statSync(dirPath);
  } catch {
    return 0;
  }
  if (!stat.isDirectory()) {
    return stat.mtimeMs;
  }
  let latest = 0;
  const stack = [dirPath];
  while (stack.length) {
    const dir = stack.pop();
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const ent of entries) {
      const full = path.join(dir, ent.name);
      let s;
      try {
        s = fs.statSync(full);
      } catch {
        continue;
      }
      if (s.isDirectory()) {
        if (recursive) stack.push(full);
        continue;
      }
      if (s.mtimeMs > latest) latest = s.mtimeMs;
    }
  }
  return latest;
}

// 递归查找目录下 mtime 最新的文件（可带后缀过滤），返回 {path, mtimeMs}
function newestFile(dirPath, { ext, maxDepth = 6 } = {}) {
  const root = dirPath;
  let best = null;
  const stack = [[root, 0]];
  while (stack.length) {
    const [dir, depth] = stack.pop();
    if (depth > maxDepth) continue;
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const ent of entries) {
      const full = path.join(dir, ent.name);
      let s;
      try {
        s = fs.statSync(full);
      } catch {
        continue;
      }
      if (s.isDirectory()) {
        stack.push([full, depth + 1]);
        continue;
      }
      if (ext && !ent.name.endsWith(ext)) continue;
      if (!best || s.mtimeMs > best.mtimeMs) {
        best = { path: full, mtimeMs: s.mtimeMs };
      }
    }
  }
  return best;
}

// 多会话聚合：返回最近活跃的文件列表（按 mtime 降序）。
// windowMs>0 时只保留该时间窗内有写入的文件；limit>0 截断数量。
function recentFiles(dirPath, { ext, maxDepth = 6, windowMs = 0, limit = 10 } = {}) {
  const files = [];
  const stack = [[dirPath, 0]];
  while (stack.length) {
    const [dir, depth] = stack.pop();
    if (depth > maxDepth) continue;
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const ent of entries) {
      const full = path.join(dir, ent.name);
      let s;
      try {
        s = fs.statSync(full);
      } catch {
        continue;
      }
      if (s.isDirectory()) {
        stack.push([full, depth + 1]);
        continue;
      }
      if (ext && !ent.name.endsWith(ext)) continue;
      if (windowMs > 0 && Date.now() - s.mtimeMs > windowMs) continue;
      files.push({ path: full, mtimeMs: s.mtimeMs });
    }
  }
  files.sort((a, b) => b.mtimeMs - a.mtimeMs);
  if (limit > 0 && files.length > limit) files.length = limit;
  return files;
}

// 读取文件尾部 maxBytes 字节，按行切分，尽量返回完整行
function tailLines(filePath, maxBytes = 16 * 1024) {
  let fd;
  try {
    fd = fs.openSync(filePath, 'r');
    const size = fs.fstatSync(fd).size;
    const start = Math.max(0, size - maxBytes);
    const buf = Buffer.alloc(size - start);
    fs.readSync(fd, buf, 0, buf.length, start);
    let text = buf.toString('utf8');
    // 丢掉可能被截断的首行
    const firstNl = text.indexOf('\n');
    if (start > 0 && firstNl >= 0) text = text.slice(firstNl + 1);
    const lines = text.split('\n').filter((l) => l.trim().length > 0);
    return lines.map((l) => {
      try {
        return JSON.parse(l);
      } catch {
        return null;
      }
    }).filter((x) => x !== null);
  } catch {
    return [];
  } finally {
    if (fd !== undefined) {
      try { fs.closeSync(fd); } catch {}
    }
  }
}

// 读取普通文本日志尾部；与 tailLines 分开，避免把非 JSON 日志全部丢弃。
function tailTextLines(filePath, maxBytes = 32 * 1024) {
  let fd;
  try {
    fd = fs.openSync(filePath, 'r');
    const size = fs.fstatSync(fd).size;
    const start = Math.max(0, size - maxBytes);
    const buf = Buffer.alloc(size - start);
    fs.readSync(fd, buf, 0, buf.length, start);
    let text = buf.toString('utf8');
    const firstNl = text.indexOf('\n');
    if (start > 0 && firstNl >= 0) text = text.slice(firstNl + 1);
    return text.split('\n').filter((line) => line.trim().length > 0);
  } catch {
    return [];
  } finally {
    if (fd !== undefined) {
      try { fs.closeSync(fd); } catch {}
    }
  }
}

// 只读 SQLite 查询（调用系统 sqlite3，零依赖；WAL 只读模式不锁库）
function sqliteQuery(dbPath, sql) {
  if (!fs.existsSync(dbPath)) {
    throw new Error(`db not found: ${dbPath}`);
  }
  const args = ['-readonly', dbPath, sql];
  const out = execFileSync('/usr/bin/sqlite3', args, {
    stdio: ['ignore', 'pipe', 'ignore'],
    timeout: 3000,
    encoding: 'utf8',
  });
  return out;
}

// 端口是否监听（lsof -i :port）
function portListening(port) {
  try {
    const out = execFileSync('/usr/sbin/lsof', ['-nP', '-i', `TCP:${port}`], {
      stdio: ['ignore', 'pipe', 'ignore'],
      timeout: 3000,
    }).toString();
    return out.includes('LISTEN');
  } catch {
    return false;
  }
}

function nowMs() {
  return Date.now();
}

module.exports = {
  isProcessRunning,
  isProcessRunningPattern,
  isProcessMatching,
  recentMtimeMs,
  newestFile,
  recentFiles,
  tailLines,
  tailTextLines,
  sqliteQuery,
  portListening,
  nowMs,
};
