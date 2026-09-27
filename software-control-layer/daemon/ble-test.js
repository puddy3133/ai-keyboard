// macOS BLE 兼容性实测：扫描键盘 BLE → 连接 → 发现 Config 服务 → 写 8 字节帧
// 这是 BLE 灯控通道的关键硬门槛验证。
'use strict';

const noble = require('@abandonware/noble');

// 固件源码 UUID（128-bit 大端表示，与 CBUUID 一致）
const CONFIG_SVC = '7d2f4d10-6b6f-4a2d-8b01-6d4653320001';
const MULTI_AGENT_WRITE = '7d2f4d10-6b6f-4a2d-8b01-6d4653320005';

const TARGET_NAME = 'EasyInput AI';
let found = false;

console.log('=== macOS BLE 灯控通道实测 ===');
console.log(`目标: ${TARGET_NAME}  Config服务: ${CONFIG_SVC}  多灯特征: ${MULTI_AGENT_WRITE}`);
console.log('等待蓝牙就绪...');

noble.on('stateChange', async (state) => {
  console.log(`蓝牙状态: ${state}`);
  if (state !== 'poweredOn') return;
  try {
    // 通用扫描（不按服务过滤，排除过滤条件过严的可能）
    await noble.startScanningAsync([], false);
    console.log('已开始通用扫描（无服务过滤）...');
  } catch (e) {
    console.log(`启动扫描失败: ${e.message}`);
  }
});

noble.on('scanStop', () => {
  console.log('扫描停止');
});

noble.on('discover', async (peripheral) => {
  const name = peripheral.advertisement?.localName;
  const uuids = peripheral.advertisement?.serviceUuids || [];
  // 显示所有发现的设备，便于判断键盘是否在广播
  console.log(`[发现] name=${name || '(无名称)'} id=${peripheral.id.slice(0,8)} addr=${peripheral.address} svcUuids=[${uuids.join(',')}]`);
  const matched =
    name === TARGET_NAME ||
    (name || '').toLowerCase().includes('easy') ||
    uuids.some((u) => u.toLowerCase() === CONFIG_SVC);
  if (!matched) return;

  found = true;
  console.log(`\n>>> 匹配到键盘 ${TARGET_NAME}，尝试连接...`);
  try {
    await noble.stopScanningAsync();
    await peripheral.connectAsync();
    console.log('连接成功!');

    // 全量发现所有服务与特征（noble uuid 为小写无连字符格式）
    await peripheral.discoverAllServicesAndCharacteristicsAsync();
    const services = peripheral.services || [];
    console.log(`发现服务: ${services.length} 个`);
    let cfgSvc = null;
    let multiWriteChar = null;
    for (const svc of services) {
      const chars = svc.characteristics || [];
      console.log(`  服务 ${svc.uuid}: ${chars.length} 个特征`);
      for (const c of chars) {
        console.log(`    特征 ${c.uuid}  props=${(c.properties || []).join(',')}`);
        if (c.uuid.toLowerCase().endsWith('0005')) multiWriteChar = c;
      }
      if (svc.uuid.toLowerCase().includes('7d2f4d10')) cfgSvc = svc;
    }

    if (!multiWriteChar) {
      console.log('!! 未找到 0x05 多灯特征（可能固件未烧录 BLE 版）');
      process.exit(1);
    }
    console.log(`\n>>> 找到多灯特征 ${multiWriteChar.uuid}，写入 8 字节帧...`);
    const frame = Buffer.from([0x16, 0x01, 0, 0, 0, 0, 0, 0]);
    console.log(`写入帧: [${[...frame].join(',')}]`);
    await multiWriteChar.writeAsync(frame, false);
    console.log('写入成功 ✓  BLE 灯控通道可用！');

    // 心跳帧（bit0）
    const hb = Buffer.from([0x16, 0x01, 0, 0, 0, 0, 0, 0x01]);
    await multiWriteChar.writeAsync(hb, false);
    console.log('心跳帧写入成功 ✓');

    await peripheral.disconnectAsync();
    console.log('已断开连接，实测完成');
    process.exit(0);
  } catch (e) {
    console.log(`!! 连接/写入失败: ${e.message}`);
    console.log('（可能被系统或官方 App 独占，详见错误信息）');
    process.exit(2);
  }
});

// 超时保护
setTimeout(() => {
  if (!found) {
    console.log('!! 60s 未发现键盘（可能蓝牙未开、或已被其它连接独占而不广播）');
    process.exit(3);
  }
}, 60000);
