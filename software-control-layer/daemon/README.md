# ai-keyboard-light — AI 键盘灯控守护进程

EasyInput AI 键盘（ESP32-S3）5 灯独立关联 5 个 AI 应用状态，经 **CDC 串口**驱动 WS2812 灯。
零侵入（不改任何应用配置），纯本地运行，不依赖官方 EasyInput.app。

## 安装

```bash
cd ai-keyboard-light
npm install        # 安装 node-serialport（唯一第三方依赖）
```

## 使用

```bash
node kb-light.js start      # 启动守护进程（后台）
node kb-light.js status     # 查看 5 应用 + 5 灯状态
node kb-light.js stop       # 停止
```

### CLI 命令

| 命令 | 说明 |
|---|---|
| `kb-light start` / `stop` / `status` | 守护进程生命周期 |
| `kb-light apps` | 只查看 5 应用状态 |
| `kb-light set L1 working` | 手动设置灯状态（working/waiting/done/error/idle/offline） |
| `kb-light auto L1` | 恢复该灯自动采集 |
| `kb-light off` | 一键全灭（下次状态变化自动恢复） |
| `kb-light test` | 自检：5 灯依次播放灯效序列 |
| `kb-light map L1 codex` | 修改灯映射（codex/claude/openclaw/doubaowork/deepseek） |
| `kb-light mapping` | 查看当前映射 |

### 缺省灯映射（可用 `kb-light map` 自定义）

| 灯 | 应用 | 说明 |
|---|---|---|
| L1 | openclaw | OpenClaw TUI |
| L2 | codex | CodeX CLI / Desktop / ChatGPT 联动 |
| L3 | claude | Claude CLI |
| L4 | deepseek | Deepseek Harness（127.0.0.1:3080） |
| L5 | doubaowork | 豆包工作桌面端 |

## 状态与灯效

| 状态 | 颜色 | 灯效 | 触发条件（简） |
|---|---|---|---|
| 工作中 | 蓝 | 呼吸 | 应用 30s 内有活动（推理/调工具/文件写入） |
| 待确认 | 橙 | 闪烁 | Desktop 未读 / 待用户输入确认 |
| 完成 | 绿 | 常亮 30s 后自灭 | 任务/回复完成，30 秒后自动熄灭回待命 |
| 错误 | 红 | 快闪 | 工具输出 error/failed、任务失败 |
| 待命/离线 | 灭 | 关闭 | 在线无任务或应用未运行 |

## 配置

配置文件：`~/.ai-keyboard-light/config.json`（首次启动自动生成）。
含灯映射、各采集器路径、串口参数（VID=0x303a / 115200）、HTTP 端口（8765）、心跳间隔（10s）。

## HTTP API（127.0.0.1:8765）

- `GET /api/health` — 运行状态
- `GET /api/status` — 应用状态 + 灯状态
- `GET /api/lights` — 灯状态
- `POST /api/lights/L1 {"state":"working"}` — 手动设灯（`"auto"` 恢复）

## 架构

```
5 应用状态源（JSONL/SQLite/mtime/进程）
        ↓ 2s 轮询（零侵入采集器）
   daemon 聚合 + 灯映射（可自定义/可手动覆盖）
        ↓ 8 字节 0x16 帧（10s 心跳保活）
   CDC 串口 → 固件 → WS2812×5 独立灯
```

## 验证

- `node test-collectors.js` — 用本机真实状态文件验证 5 采集器
- `node test-unit.js` — 帧协议/聚合/覆盖单元测试
