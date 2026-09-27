# 源码、资源与依赖清单

本文记录 EasyInput Maker 当前首版候选的可发布内容及其验证层级。

## 源码与构建入口

| 路径 | 用途 | 许可处理 |
| --- | --- | --- |
| `CMakeLists.txt`、`sdkconfig.defaults`、`partitions.csv` | ESP32-S3 构建、内存与分区合同 | 根目录项目许可证 |
| `main/` | 应用入口与 ESP-IDF 平台适配 | 根目录项目许可证 |
| `components/keyboard/` | 输入、配置、状态、传输、电源和音频公共逻辑 | 根目录项目许可证 |
| `components/esp_hid/` | 修改后的 ESP-IDF HID 适配 | Apache-2.0；来源与修改见 `components/esp_hid/UPSTREAM.md` |
| `features/speaker_assets/` | 声音格式、读取、A/B 存储、会话、同步和运行时 | 根目录项目许可证 |
| `diagnostics/` | 可选扬声器、IMA-ADPCM 与 Ogg/Opus 诊断 | 项目源码与独立依赖分别按其许可证 |
| `host_test/` | 主机行为、协议、资源与源码合同测试 | 根目录项目许可证 |

## 项目自有资源

| 资源 | 来源 | 固定指纹 |
| --- | --- | --- |
| `features/speaker_assets/assets/waytoagi.eiad` | 项目自有 WaytoAGI 出厂提示音；固定 EIAD v1 资源 | SHA-256 `f29312efa6cb78eb1ac43ca762acbbfefa81769f00dee0930f81fd53bc311751` |
| `diagnostics/speaker_ima_adpcm_probe/assets/easyinput_boot_probe_eiad.h` | 独立项目自有诊断音的固定 EIAD 数据 | EIAD 数据 SHA-256 `c483ead293fba5321e5b22e7cfff699e56b3db8d7c157ec4b23eba8b8bd2de42` |
| `diagnostics/speaker_opus_probe/assets/easyinput_boot_probe.ogg` | 同一独立诊断音经 libopus 编码并封装为 Ogg | SHA-256 `d49de654f1b72f05a835299801950df202688f7adf70bbfac11d2dbb48941411` |

这些资源均为项目自有材料，适用根目录非商业许可证；来源、转换信息和固定指纹见各 `assets/README.md`。

## 锁定依赖

| 依赖 | 版本 | 用途 |
| --- | --- | --- |
| ESP-IDF | 5.5.5 | 固件构建框架 |
| `espressif/esp_tinyusb` | 1.7.6~2 | USB HID 托管组件 |
| `espressif/tinyusb` | 0.21.0~1 | USB 传递依赖 |
| `espressif/esp_audio_codec` | 2.5.0 | 仅在 Ogg/Opus 诊断构建启用 |

具体通知和适用许可证见 `THIRD_PARTY_NOTICES.md`。默认构建的依赖锁为 `dependencies.lock`；Opus 诊断使用独立构建目录中的锁文件，避免改变默认依赖图。

## 不进入版本控制的内容

- `build*`、`managed_components/`、生成的 `sdkconfig`、二进制、映射、缓存与日志；
- Wi-Fi 密码、同步密钥、账号、设备标识和本机配置；
- 制造资料、运维资料和没有明确再发布权的素材。

## 当前验证证据

- 主机测试：60/60 通过；
- 默认 ESP-IDF 5.5.5 / ESP32-S3 构建：通过；具体镜像大小、分区余量和固件 SHA-256 以对应 GitHub Release 的正式构建记录为准；链接映射包含 WaytoAGI EIAD 资源起止符号；
- IMA-ADPCM 与 Ogg/Opus 诊断构建：待在默认固件验证后分别重验；
- 板级静态检查：0 FAIL；由于 v2 引脚声明位于条件编译分支，检查器报告 2 个 WARN，不能据此宣称全部引脚已被该静态工具验证；
- 公开文件扫描：未发现未处理的凭据、私钥、本机绝对路径、设备端口或构建产物；测试中的固定网络值和密钥均为显式测试向量；
- EasyInput V2.0 实板联合功能测试：当前公开候选已完成烧录、正常启动、输入、灯光、USB/BLE、音频、电源与恢复主流程测试，观察正常；麦克风端到端链路使用兼容的本地 companion 和受信任本地网络。

静态检查、主机测试、固件构建、烧录、运行日志和物理现象是不同层级的证据，不能互相替代。
