# 开始使用 EasyInput Maker

这条路径的第一个目标不是立刻改代码，而是先证明“仓库、依赖和本机环境可以共同构建一份 ESP32-S3 固件”。完成后再进入 AI 修改和实板验证。

## 你需要准备

- EasyInput V2.0 开发板；固件别名 `v2`、PCB 丝印 AI Keyboard V2.1 都指这块板；
- 支持数据传输的 USB 线；
- Git；
- ESP-IDF **5.5.5**，目标工具链包含 `esp32s3`；
- 项目路径和 ESP-IDF 安装路径均不要包含空格。

ESP-IDF 的安装与环境激活请使用乐鑫官方的 [ESP-IDF 5.5.5 ESP32-S3 入门文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/index.html)。本项目依赖版本记录在 `main/idf_component.yml` 和 `dependencies.lock` 中。

## 1. 下载源码

只想直接使用固件或在本地完成课程练习，可以下载官方仓库：

```bash
git clone https://github.com/CY-CHENYUE/easy-input-maker.git
cd easy-input-maker
```

不要把 Wi-Fi 密码、同步密钥或本机配置提交到仓库。构建目录、`sdkconfig`、密钥文件和常见本机配置已由 `.gitignore` 默认排除。

如果已经确定要向社区贡献代码，建议先在 GitHub 上 Fork，再下载自己的 Fork。两种下载方式及后续提交步骤见 [中文共创与提交教程](../contributing/how-to-contribute.md)。

## 2. 确认工具链

在已经激活 ESP-IDF 的终端中运行：

```bash
idf.py --version
```

预期版本是 `ESP-IDF v5.5.5`。如果命令不存在，说明当前终端尚未加载 ESP-IDF 环境；先按你的安装方式激活，再继续。

## 3. 构建默认固件

```bash
idf.py build
```

第一次构建会解析锁定组件并编译较多文件，耗时比增量构建更长。看到 `Project build complete` 表示构建成功，默认应用镜像名为 `easy_input_keyboard.bin`；该名称与 USB/BLE 的 `EasyInput AI` 身份一起用于兼容既有设备与 companion，不改变公开仓库名称 EasyInput Maker。构建只证明源码和工具链可以生成固件，不证明板上按键、灯光、无线或音频已经实际工作。

默认构建关闭扬声器专项诊断。默认固件嵌入项目自有的 WaytoAGI 出厂提示音；IMA-ADPCM 与 Ogg/Opus 诊断夹具是独立的项目自有测试音，不是出厂提示音。来源和固定指纹记录在对应 `assets/README.md` 中。

本仓库是固件项目，不包含桌面 companion 应用。构建固件不要求 companion；需要体验麦克风 Wi-Fi 上行时，另需实现或连接兼容 `docs/security/audio-control-v1.md` 的本地 companion，并只在受信任的本地网络使用。

## 4. 在电脑上运行测试

宿主测试不连接开发板，适合 AI 每次修改后快速验证协议、配置、输入和状态逻辑：

```bash
cmake -S host_test -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

测试数量会随项目演进变化，应以你实际运行的 CTest 输出为准。

## 5. 下一步

- 要装到自己的开发板：阅读 [烧录与恢复](flash-and-recovery.md)。
- 要用 AI 增加功能：阅读 [AI / Vibe Coding 教学路径](../teaching/ai-vibe-coding.md)。
- 要提出想法或贡献修改：阅读 [中文共创与提交教程](../contributing/how-to-contribute.md)。
- 要改灯光、音频、电源或 BOOT：先阅读 [硬件安全边界](../hardware/easyinput-v2-safety.md)。

遇到问题时，Issue 中应提供最小复现、ESP-IDF 版本、板型和实际运行过的命令；删除个人路径、设备标识、网络凭据和无关日志。
