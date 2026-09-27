// 灯状态定义与固件 CDC 帧值映射
'use strict';

// 设计文档 4.1 的应用级状态（6 态）
const APP_STATE = {
  OFFLINE: 'offline', // 应用不在线 / 灯未映射
  IDLE: 'idle',       // 在线待命
  WORKING: 'working', // 工作中
  WAITING: 'waiting', // 待确认
  DONE: 'done',       // 完成
  ERROR: 'error',     // 错误
};

// 固件 cdc_light_control 帧值：
//   0=idle/灭  1=running蓝  2=waiting橙  3=done绿  4=failed红
// offline 与 idle 都映射为 0（灭灯）
const STATE_TO_FRAME = {
  offline: 0,
  idle: 0,
  working: 1,
  waiting: 2,
  done: 3,
  error: 4,
};

// 灯效（供 HTTP API / 日志参考；固件端实际颜色由状态决定）
const EFFECTS = {
  idle: { color: [0, 0, 0], animation: 'off' },
  working: { color: [0, 100, 255], animation: 'breathing' },
  waiting: { color: [255, 140, 0], animation: 'blinking' },
  done: { color: [0, 200, 0], animation: 'steady' },
  error: { color: [255, 0, 0], animation: 'fast_blink' },
  offline: { color: [0, 0, 0], animation: 'off' },
};

// 多会话聚合优先级（同一应用多个会话时，取优先级最高的状态）
// 用户确认：红 > 黄 > 绿 > 蓝 > 灭
const STATE_PRIORITY = {
  error: 4,    // 红，最高
  waiting: 3,  // 黄
  done: 2,     // 绿
  working: 1,  // 蓝
  idle: 0,     // 灭
  offline: 0,  // 灭
};

module.exports = { APP_STATE, STATE_TO_FRAME, EFFECTS, STATE_PRIORITY };
