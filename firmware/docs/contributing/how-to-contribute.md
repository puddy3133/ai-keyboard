# EasyInput Maker 中文共创与提交教程

使用这个仓库有三种方式：直接使用原固件、在本地自己修改、把想法或修改贡献给社区。你不需要先会完整的 C++，可以让 AI 帮助阅读、编码和验证，但仍需确认需求、改动范围、测试证据和实板现象。

## 先判断你现在想做什么

| 目的 | 从哪里开始 | 是否需要 Issue / PR |
| --- | --- | --- |
| 直接构建、烧录和使用原固件 | 下载官方仓库，按照开始使用文档操作 | 不需要 |
| 自己修改或完成课程练习 | 下载官方仓库，创建本地练习分支，让 AI 修改并验证 | 不需要，可以只保留在本地 |
| 提出新想法或反馈问题 | 搜索已有 Issue，再选择功能建议或问题反馈模板 | 需要 Issue，不要求先写代码 |
| 把已经完成的修改贡献给社区 | Fork 仓库，在功能分支中修改、验证并提交 PR | 需要 Pull Request；简单修改不强制先开 Issue |

所有方式都受仓库许可证约束。自己修改不等于可以将固件用于未经授权的商业用途。

## 只想下载、使用或自己修改

下载官方仓库：

```bash
git clone https://github.com/CY-CHENYUE/easy-input-maker.git
cd easy-input-maker
```

只使用原固件时，按照 [开始使用](../getting-started/README.md) 完成构建，并在确认目标开发板后按照 [烧录与恢复](../getting-started/flash-and-recovery.md) 操作。

要自己修改或完成课程练习，建议先创建一个本地练习分支，避免把原始 `main` 和自己的实验混在一起：

```bash
git switch -c practice/led-brightness
```

然后按照 [AI / Vibe Coding 教学路径](../teaching/ai-vibe-coding.md) 让 AI 修改、运行测试、完成构建并记录实板观察。只在自己的电脑和开发板上使用时，不需要提交 Issue、推送分支或创建 PR。

如果修改结果后来值得贡献给社区，可以继续使用现有文件，不需要重新让 AI 做一遍；按照下方“[已经下载官方仓库并修改，后来想贡献](#已经下载官方仓库并修改后来想贡献)”完成转换即可。

## 有想法或发现问题：提交 Issue

1. 打开仓库的 [Issues](https://github.com/CY-CHENYUE/easy-input-maker/issues)，先搜索是否已有相同建议或问题。
2. 选择 [New issue](https://github.com/CY-CHENYUE/easy-input-maker/issues/new/choose)。
3. 新功能选择 **Feature request**，至少说明：
   - 学员或 Maker 想完成什么；
   - 一个具体使用场景；
   - 最终可以观察到什么结果；
   - 可能涉及按键、旋钮、灯光、USB/BLE、音频、电源或配置中的哪些部分。
4. 如果是已有功能不正常，选择 **Bug report**，填写预期现象、实际现象、最小复现步骤、固件版本、ESP-IDF 版本和已经完成的验证。
5. 提交后等待维护者确认范围、风险和优先级；Issue 中的讨论和证据都会公开保留。

只提出需求不要求先写代码。请描述“想解决什么”和“希望看到什么”，不要把未经审核的引脚、协议或安全方案直接当成确定实现。

如果问题可能泄露凭据、设备身份或带来安全风险，不要创建公开 Issue，按照 [安全报告说明](../../SECURITY.md) 私下提交。

## 已经完成修改并想贡献：提交 Pull Request

公开仓库采用“个人 Fork + 功能分支 + Pull Request”的方式接收外部代码。下面以增加灯光亮度功能为例。

### 1. Fork 并下载仓库

在 GitHub 仓库页面点击 **Fork**，把仓库复制到自己的 GitHub 账号，然后下载自己的 Fork：

```bash
git clone https://github.com/<你的 GitHub 用户名>/easy-input-maker.git
cd easy-input-maker
```

`<你的 GitHub 用户名>` 需要替换成自己的账号名称，不要原样复制。

### 2. 为这次修改创建分支

不要直接在 `main` 上开发。一次分支只处理一个清楚的问题：

```bash
git switch -c feat/led-brightness
```

常见分支前缀：

- `feat/`：增加功能；
- `fix/`：修复问题；
- `docs/`：修改文档；
- `test/`：补充测试；
- `practice/`：只保留在本地的个人练习。

### 3. 让 AI 在公开边界内修改

可以把下面的任务交给仓库中的 AI Agent：

```text
请先阅读 AGENTS.md、AI_DEVELOPMENT.md、CONTRIBUTING.md 和本次任务相关文档。
只完成我描述的一个可观察结果，不修改无关功能。
先说明将修改哪些文件和涉及哪些硬件资源，再实现代码和测试。
运行适用的宿主测试与 ESP-IDF build，区分静态检查、构建成功和实板观察。
不要写入凭据、设备标识、个人路径、私有日志或内部资料。
完成后列出实际改动、已运行验证、未运行验证和烧录后需要观察的现象。
未经我明确要求，不要提交、推送或创建 Pull Request。
```

涉及 GPIO、BOOT、GPIO8、USB、BLE、麦克风、扬声器、电源、睡眠、网络认证或持久配置时，先阅读对应安全文档，并建议先通过 Issue 与维护者确认方案。

### 4. 检查修改和验证证据

先查看 AI 实际改了什么：

```bash
git status --short
git diff
```

运行适用的宿主测试：

```bash
cmake -S host_test -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

在已经激活 ESP-IDF 5.5.5 的终端中完成固件构建：

```bash
idf.py build
```

构建通过不等于实板功能通过。如果修改了物理行为，需要说明是否烧录，以及在 EasyInput V2.0 上实际观察到了什么；没有实板验证时如实写“未进行实板验证”。

### 5. 提交本次修改

只添加本次任务相关文件，不使用 `git add .` 把其他内容一起带进去：

```bash
git add <本次修改的文件>
git diff --staged
git commit -m "feat: add adjustable LED brightness"
```

常用提交类型包括 `feat`、`fix`、`docs`、`test`、`refactor`、`ci` 和 `chore`。提交说明应简短表达这次修改完成了什么。

### 6. 推送自己的功能分支

```bash
git push -u origin feat/led-brightness
```

提交和推送是两个独立动作。让 AI 操作时，应明确说“提交”“推送”或“提交并推送”，并在执行前检查它准备包含的文件。

### 7. 创建 Pull Request

推送后，在自己的 Fork 页面点击 **Compare & pull request**：

- 目标仓库选择 `CY-CHENYUE/easy-input-maker`；
- 目标分支选择 `main`；
- 来源选择自己的 Fork 和本次功能分支；
- 按 PR 模板说明修改内容、用户可见效果、硬件与协议影响、运行过的命令、测试结果、实板观察、AI 协助范围和第三方材料来源。

存在相关 Issue 时，在 PR 中写 `Closes #编号`；没有相关 Issue 也可以提交边界清楚的小型 PR。

### 8. 根据审核继续修改

维护者可能要求缩小改动、增加测试、补充来源说明或完成实板验证。在同一分支继续修改、提交并推送，新提交会自动更新原来的 PR，不需要重新创建。

PR 只有在维护者审核通过后才会合并到 `main`。代码能够编译不代表一定会被接受；硬件安全、许可证、维护成本和项目的非商业定位都会纳入审核。

## 已经下载官方仓库并修改，后来想贡献

如果最初运行的是：

```bash
git clone https://github.com/CY-CHENYUE/easy-input-maker.git
```

此时本地的 `origin` 指向官方仓库，普通学员通常不能直接向官方 `main` 推送。但现有修改不需要丢弃或重做，可以按下面步骤转到自己的 Fork。

### 1. 先在 GitHub 创建 Fork

打开官方仓库页面，点击 **Fork**，在自己的 GitHub 账号下创建 `easy-input-maker`。

### 2. 在现有项目中保留当前修改

先检查当前状态：

```bash
git status --short
git branch --show-current
git remote -v
```

如果当前仍在 `main`，为已有修改创建功能分支。创建分支不会清除当前未提交修改；把名称换成实际任务：

```bash
git switch -c feat/my-change
```

如果已经在 `feat/`、`fix/`、`docs/` 或其他自己的分支上，不要重复创建，继续使用现有分支即可。

### 3. 把官方仓库和个人 Fork 分开

将原来的官方地址保留为 `upstream`，再把自己的 Fork 设置为 `origin`：

```bash
git remote rename origin upstream
git remote add origin https://github.com/<你的 GitHub 用户名>/easy-input-maker.git
git remote -v
```

确认 `origin` 指向自己的账号、`upstream` 指向 `CY-CHENYUE/easy-input-maker`。不要把 `<你的 GitHub 用户名>` 原样复制进命令。

### 4. 检查、提交并推送

如果修改还没有提交，先按照前面的验证与提交步骤处理；然后推送当前功能分支：

```bash
git push -u origin HEAD
```

这里的 `HEAD` 表示当前分支，可以避免把示例分支名误当成自己的真实分支。最后从个人 Fork 的功能分支向 `CY-CHENYUE/easy-input-maker` 的 `main` 创建 Pull Request。这个过程不需要 force push，也不需要删除、重置或重新下载当前修改。

如果最初下载的是 GitHub ZIP 而不是 `git clone`，该目录没有 Git 历史。先 Fork 并把自己的 Fork Clone 到一个新目录，只复制确实修改过的源码、测试和文档文件；不要复制构建目录、`sdkconfig`、凭据或日志。检查 `git diff` 后，再按照正常 PR 流程提交。

## 提交前快速检查

- [ ] 一次 Issue 或 PR 只解决一个清楚的问题。
- [ ] 没有凭据、设备标识、个人路径、私有日志或内部资料。
- [ ] AI 生成内容已经由提交者阅读、理解并检查。
- [ ] 已写明实际运行的测试、构建和实板验证，没有把它们混为一谈。
- [ ] 修改硬件行为时已阅读 [EasyInput V2.0 硬件安全边界](../hardware/easyinput-v2-safety.md)。
- [ ] 第三方代码和素材来源清楚，并保留对应许可证与声明。
- [ ] 已阅读 [完整贡献与许可规则](../../CONTRIBUTING.md)。

提交贡献不会自动转移贡献者版权，也不会额外授予商业许可。项目自有材料和第三方材料分别遵守仓库中已经适用的许可证，具体以 [CONTRIBUTING.md](../../CONTRIBUTING.md)、[LICENSE](../../LICENSE) 和 [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) 为准。
