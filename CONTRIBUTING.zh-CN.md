# 为 nsg 贡献

首先，感谢你花时间为这个项目做出贡献！

本指南说明了如何构建项目、报告 bug 以及提交更改。请同时阅读：

- [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) —— 我们如何对待彼此
- [STYLE_GUIDE.md](STYLE_GUIDE.md) —— 代码风格规范（仅英文）
- [LICENSE](LICENSE) —— AGPL-3.0

> 本文是英文版 [CONTRIBUTING.md](CONTRIBUTING.md) 的中文参考翻译，仅供参考。如有任何出入，以英文版为准。

## 项目结构

- `android/` —— Android 应用，通过蓝牙智能设备模式向尼康 Z 系列相机提供 GPS 数据。
- `esp32/` —— ESP32 固件，即最终产品。支持的开发板与构建环境请参阅 `esp32/README.md`。
- `kotlin-poc/` —— Linux 上的 PoC（需要 BlueZ）。**已弃用**：不再是开发重点，但仍接受贡献。
- `doc/` —— 协议逆向工程笔记（`doc/nikon-z-gps.md`）与 Android PoC 设计文档（`doc/android-poc.md`）。

开发重点在 `android/` 和 `esp32/`。

## 开始之前

### Android 应用

环境要求：

- JDK 17 或更新版本（建议使用较新的 LTS，如 17 或 21）
- Android SDK，包含 platform 36（或者直接使用 Android Studio）
- Gradle 由 wrapper 提供（`android/gradlew`）

构建：

```sh
cd android
./gradlew :app:assembleDebug
```

运行单元测试：

```sh
./gradlew :app:testDebugUnitTest
```

调试 APK 生成于 `android/app/build/outputs/apk/debug/`。你也可以在 Android Studio 中打开 `android/` 目录并直接运行到设备上。

### ESP32 固件

环境要求：

- [pioarduino](https://github.com/pioarduino/platform-espressif32)
  - 官方 PlatformIO 无法使用，因为它不支持最新的 espressif32 框架
- 开发板平台与库已在 `esp32/platformio.ini` 中锁定

支持的构建环境（详见 `esp32/README.md`）：

| 环境 | 开发板 |
|---|---|
| `m5stack-core2-release` / `m5stack-core2-debug` | M5Stack Core2 |
| `esp32-wroom-32e-release` / `esp32-wroom-32e-debug` | ESP32 WROOM 32E |
| `native` | 宿主机（单元测试） |

构建：

```sh
cd esp32
pio run -e esp32-wroom-32e-release
```

运行单元测试（单元测试的位置与运行方式见 [STYLE_GUIDE.md](STYLE_GUIDE.md)）：

```sh
pio test -e native
```

### kotlin-poc（已归档）

```sh
cd kotlin-poc
./gradlew run   # 需要 Linux 与 BlueZ
```

## 报告 bug

在新建 issue 之前，请先搜索已有的 issue。

报告 bug 时请包含：

- **硬件**：手机型号（Android 应用）或开发板型号（ESP32），以及涉及的相机型号
- **环境**：Android 版本 / 应用版本，或 ESP32 构建环境与固件版本；如已知，附上相机固件版本
- **复现步骤**，包括相机之前是否与 SnapBridge 配对过
- **日志**：Android 使用 `adb logcat` 输出；ESP32 使用串口监视器输出（115200 波特率）
- **预期行为与实际行为**

欢迎提供截图、录屏以及 ESP32 屏幕的照片。

## 提交更改（PR 工作流程）

> 强烈建议在实现任何功能之前先创建一个 issue。

1. Fork 本仓库，并创建描述性的分支名（例如 `fix/ble-reconnect`、`feature/xxx`）。
2. 保持改动聚焦：一个 PR 只做一个逻辑改动。
3. 在本地运行相关测试（见上文），并确保全部通过。
4. 如果改动影响用户可见行为或开发板支持，请同步更新文档（`README.md`、`esp32/README.md`、`doc/`）。
5. 如果你还没有加入 [CONTRIBUTORS.md](CONTRIBUTORS.md)，请将你自己添加进去。
6. 向 `master` 分支发起 PR。
7. 如果 PR 中任何环节使用了 LLM/AI，**必须**声明（见下方 LLM/AI 政策）。
8. 提交信息必须使用英文（见 STYLE_GUIDE.md）。

代码评审由维护者利用业余时间进行。**维护者不会从这个项目中获得任何报酬。每个人都是自愿的，没有义务回答问题或评审 PR。** 我们会尽力而为，但我们不欠你什么。

## 语言政策

- 英语是本项目的首要语言：代码、注释、提交信息与文档均使用英语；当存在多语言翻译时，应视为唯一事实来源。
- 对于 issue 和 PR，鼓励使用英语，但中文同样可以。如果使用中文发帖，欢迎附上英文翻译（非必须）——翻译工具和 LLM 使得这件事变得很容易。
- 代码与代码注释必须使用英语，除非该代码属于本地化机制的一部分（例如 UI 字符串）。

## LLM/AI 政策

本项目允许使用 LLM 和 AI 助手辅助编码与项目工作——但必须负责任地使用：

- **禁止 vibe coding。** 你不得在不理解代码含义的情况下，用 LLM/AI 生成代码并提交。只提交你能解释清楚的工作；看不懂的代码就不要提交。
- **任何人都没有义务阅读 LLM 的输出。** 如果某个 issue 或 PR 完全由 LLM 生成，维护者和其他贡献者有权完全忽略它。
- **PR 中的 LLM 使用必须声明。** 如果你在 PR 中使用了 LLM/AI，必须在 PR 描述中明确说明：
  - 使用了哪个模型或工具；
  - 具体如何使用（例如：生成代码、撰写描述、自查改动、翻译等）。
  - 这是为了透明起见，不会影响 PR 评审。如果是一个由 LLM 生成的好 PR，我们会很高兴地接受它。
- **LLM/AI 评审仅供参考。** 维护者可以使用 LLM 辅助评审 PR，但这些输出仅供他们自己参考——绝不构成你必须修改的依据，除非人类评审者在其自己的评审中引用了这些输出。
- **最终决定必须由人类作出。** 批准或要求修改（APPROVAL / REQUEST_CHANGES）必须由人类撰写。自动或 AI 生成的评审意见永远不能作为决策依据。

## 行为准则

本项目有一份[行为准则](CODE_OF_CONDUCT.md)。参与本项目即表示你同意遵守其条款。

## 许可证

贡献即表示你同意你的贡献以 [AGPL-3.0](LICENSE) 许可证发布。
