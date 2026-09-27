// 8 字节 CDC 帧构造 / 解析
//   byte0 = magic 0x16
//   byte1 = version 0x01
//   byte2..6 = state_L1..L5（0=idle灭/1=running蓝/2=waiting橙/3=done绿/4=failed红）
//   byte7 = flags（bit0=纯心跳，仅刷新 TTL 不改状态）
'use strict';

const FRAME_MAGIC = 0x16;
const FRAME_VERSION = 0x01;
const FRAME_LEN = 8;
const FLAG_HEARTBEAT = 0x01;

// 构造完整状态帧
function buildStatusFrame(states) {
  // states: array[5] of 0..4
  const buf = Buffer.alloc(FRAME_LEN);
  buf[0] = FRAME_MAGIC;
  buf[1] = FRAME_VERSION;
  for (let i = 0; i < 5; i++) {
    const v = Number(states[i]) || 0;
    buf[2 + i] = Math.max(0, Math.min(4, v));
  }
  buf[7] = 0;
  return buf;
}

// 构造纯心跳帧（不改状态，仅刷新固件 TTL）
function buildHeartbeatFrame() {
  const buf = Buffer.alloc(FRAME_LEN);
  buf[0] = FRAME_MAGIC;
  buf[1] = FRAME_VERSION;
  buf[7] = FLAG_HEARTBEAT;
  return buf;
}

function isValidFrame(buf) {
  if (!buf || buf.length < FRAME_LEN) return false;
  return buf[0] === FRAME_MAGIC && buf[1] === FRAME_VERSION;
}

module.exports = {
  FRAME_MAGIC,
  FRAME_VERSION,
  FRAME_LEN,
  FLAG_HEARTBEAT,
  buildStatusFrame,
  buildHeartbeatFrame,
  isValidFrame,
};
