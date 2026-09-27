# 音频控制协议 v1 与安全边界

EasyInput Maker 的麦克风音频通过 Wi-Fi UDP 上行。当前 wire 版本为 `1`，用于兼容现有本地 companion。它提供来源地址限制，但不提供密码学身份认证、消息完整性或防重放能力，因此只能部署在受信任的本地网络。

## 当前信任模型

- 固件将 `audio_host` 解析为目标 IPv4 地址，并向该地址的 `audio_port` 发送心跳和音频数据；
- 只有来源 IPv4 与该目标地址一致的 EICC 控制包才会执行；其他来源返回 `Unauthorized`；
- EICC 中的 16 字节 `token` 当前仅为兼容预留，固件不会验证其内容；
- 同一已配置主机地址还可以向控制端口发送配置 JSON，随后沿用常规配置应用和持久化路径；
- 来源 IP 不是密码学身份。主机被入侵、局域网攻击、DNS 欺骗或地址伪造均可能突破这层限制。

因此，不要把该 UDP 端口暴露到互联网、访客网络或其他不可信网络；不要把真实 Wi-Fi 凭据、同步密钥、设备标识或现场配置提交到源码、Issue、PR、截图和日志中。

## 消息格式

所有多字节整数均为小端。

### EIHB 心跳（基础 20 字节）

| 偏移 | 长度 | 字段 |
| --- | ---: | --- |
| 0 | 4 | `EIHB` |
| 4 | 1 | version = 1 |
| 5 | 1 | flags：bit0 streaming，bit1 audio ready |
| 6 | 2 | reserved = 0 |
| 8 | 8 | 当前 session ID |
| 16 | 4 | heartbeat sequence |

固件允许由已接线的扩展回调把心跳扩展为 80 字节；companion 应至少识别基础 20 字节，并按实际扩展合同解析额外字段。

### EICC 控制命令（至少 36 字节）

| 偏移 | 长度 | 字段 |
| --- | ---: | --- |
| 0 | 4 | `EICC` |
| 4 | 1 | version = 1 |
| 5 | 1 | action：1 start、2 stop、3 keepalive |
| 6 | 2 | reserved |
| 8 | 8 | session ID，必须非零 |
| 16 | 4 | sequence |
| 20 | 16 | token（当前不验证，仅为兼容预留） |

解析器接受 36 字节及以上的数据报；兼容端不应依赖尾随字段被校验。合法控制消息会返回 EICA。

### EICA 确认（20 字节）

| 偏移 | 长度 | 字段 |
| --- | ---: | --- |
| 0 | 4 | `EICA` |
| 4 | 1 | version = 1 |
| 5 | 1 | action |
| 6 | 1 | status：0 OK、1 unavailable、2 bad request、3 unauthorized |
| 7 | 1 | reserved = 0 |
| 8 | 8 | session ID |
| 16 | 4 | sequence |

## 实现与验证入口

- wire 常量与结构：`components/keyboard/include/keyboard/audio_control_wire.h`；
- wire 编解码：`components/keyboard/src/audio_control_wire.cpp`；
- 网络执行、来源地址限制与配置 JSON 入口：`main/platform/keyboard_audio.cpp`；
- 结构和错误输入测试：`host_test/audio_control_wire_tests.cpp`。

如果以后升级为带认证的协议，应作为明确的兼容性变更同时修改固件、companion、测试和本文，不能只改其中一端。
