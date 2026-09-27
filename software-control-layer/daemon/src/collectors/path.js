// 路径展开（~ -> home），独立模块避免循环依赖
'use strict';

const os = require('os');
const path = require('path');

function config_expandHome(p) {
  if (typeof p !== 'string') return p;
  if (p === '~') return os.homedir();
  if (p.startsWith('~/')) return path.join(os.homedir(), p.slice(2));
  return p;
}

module.exports = { config_expandHome };
