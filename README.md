# ai-keyboard

EasyInput V2.0（ESP32-S3，8 键 + 旋钮 + 5 颗 WS2812）多 Agent 状态灯项目：电脑上运行的多个 AI 编码助手（Codex CLI、ChatGPT Desktop、Claude CLI、OpenClaw、豆包工作、DeepSeek Harness、Pi 等）的「工作中 / 待确认 / 完成 / 出错」状态，实时映射到键盘的 5 颗灯上。

目录结构：

```
ai-keyboard/
├── firmware/                  # 固件（ESP-IDF 5.5.5 / ESP32-S3）
└── software-control-layer/    # 软件控制层
    ├── daemon/                # Node.js 状态采集守护进程 + CLI（kb-light）+ HTTP API
    └── dashboard.html         # 单页灯控台（读取 daemon 本地 API，含灯效与节拍可视化）
```

## 灯语

| 状态 | 颜色 | 灯效 | 含义 |
|---|---|---|---|
| working | 蓝 | 呼吸 | 应用正在推理 / 调工具 / 生成 |
| waiting | 橙 | 慢闪 | 等待用户授权或回答 |
| done | 绿 | 快闪后常亮再自灭 | 任务完成 |
| error | 红 | 快闪 | 任务级失败（只认结构化错误信号） |
| idle / offline | 灭 | - | 待命或应用未运行 |

状态优先级：红 > 橙 > 绿 > 蓝 > 灭。每颗灯独立映射一个应用，可在 `~/.ai-keyboard-light/config.json` 中自由调整（`kb-light map L1 codex`）。

## 工作原理

```
各应用本地状态源（会话 JSONL / IndexedDB / SQLite / 进程与网络信号）
        │  零侵入只读采集（2s 轮询）
        ▼
daemon 聚合 → 灯位映射 → 8 字节 0x16 帧（10s 心跳保活）
        │  USB CDC 串口 或 BLE GATT（同一协议，二选一自动）
        ▼
固件解析帧 → 5 颗 WS2812 独立灯效
```

帧格式（8 字节）：`[0]=0x16 magic`、`[1]=0x01 version`、`[2..6]=L1..L5 状态（0-4）`、`[7]=flags（bit0=纯心跳）`。

## 快速开始

固件（需要 ESP-IDF 5.5.5）：

```bash
cd firmware
idf.py build
idf.py -p <你的串口> flash   # 烧录前请先核对设备
```

软件控制层（macOS，Node.js ≥ 18）：

```bash
cd software-control-layer/daemon
npm install
node kb-light.js start     # 启动守护进程
node kb-light.js status    # 查看状态
open ../dashboard.html     # 打开灯控台页面（读取 127.0.0.1:8765）
```

验证：

```bash
node test-unit.js          # 帧协议 / 聚合 / 采集器单测
node test-collectors.js    # 用本机真实状态文件验证各采集器
```

## 许可

本仓库分目录许可，使用时请注意区别：

- `firmware/`：EasyInput Maker 社区固件，**PolyForm Noncommercial 1.0.0**（非商业使用），原作者 CY-CHENYUE / WaytoAGI 社区项目，详见 `firmware/LICENSE` 与 `firmware/THIRD_PARTY_NOTICES.md`。
- `software-control-layer/`：灯控守护进程与页面控制层，MIT，详见 `software-control-layer/daemon/package.json`。

## 脱敏说明

本仓库为公开发布的整理版本：不包含任何本机私有配置、会话数据、日志、调试脚本或过程记录；采集器中的应用路径均为各平台默认安装位置，可通过 `~/.ai-keyboard-light/config.json` 覆盖。
