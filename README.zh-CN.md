# nsg - 尼康智能 GPS

> English README: [README.md](README.md)

Nikon SnapBridge 的替代方案：通过蓝牙 smart-device 模式（即 SnapBridge 连接时使用的模式）为 Z 系列相机提供 GPS 位置。

## 为什么？

我拥有 Z 50 II 和 Z 8 两台相机，生活在中国。中国法律规定相机一律不得内置 GPS。一个变通方案是用 SnapBridge 提供 GPS 位置。然而在我的三星手机上，它不稳定（经常与相机断开连接），而且很耗电。

买了新 Z8 之后，我想也许可以利用 10 针接口自制一个 GPS，因为它以 4800 bps 传输 NMEA-0183。但遗憾的是中国固件屏蔽了 GPS 设置，所以即使你拥有内置 GPS 的 Z9，也无法使用它。

所以对我以及其他中国用户来说，SnapBridge 是给相机喂入 GPS 并对照片进行地理标记的唯一方式。

## Android

安卓端实现已完整可用。它基于 [gkoh/furble](https://github.com/gkoh/furble) 的逆向成果，已在尼康 Z7 II 上验证可用（理论上支持所有带 SnapBridge 智能设备模式的 Z 相机）。

它可以配对全新相机并连接已保存的相机，通过蓝牙注入真实 GPS（含网络定位），以后台低功耗前台服务方式持续运行，并能自动恢复 SnapBridge 设备标识，让你在本应用与 SnapBridge 之间切换时无需删除任何配对记录。

完整文档见 [android/README.md](android/README.md)（中文版见 [android/README.zh-CN.md](android/README.zh-CN.md)）。

- 中文版：[**使用教程**](android/README.zh-CN.md#%E4%BD%BF%E7%94%A8%E6%95%99%E7%A8%8B)
- English: [**Usage Tutorial**](android/README.md#usage)

本 Android 部分由 [HowenXu](https://github.com/HowenXu) 贡献。

## Kotlin PoC（仅 Linux，需要 Bluez）

显然 Android 的 BLE 和蓝牙协议栈并不好用。把它作为 PoC 使用，违背了只专注于核心功能的干净代码的初衷。所以，作为后端开发者的我决定用自己最熟悉的东西：古老的 JVM。

目前 kotlin PoC 可以配对全新设备（已用 Z50II 和 Z8 测试）并连接已保存的设备，也可以向相机发送假的 GPS 载荷。

TODO：让它更健壮？比如相机从空闲状态唤醒时自动重连，或者同时连接多个 BLE 设备？

## ESP32（最终产品）

专用硬件运行在 ESP32 上。最初的 M5StackS3/CoreS3 方案被否决，因为 ESP32-S3 只支持 BLE，不支持蓝牙经典（Bluetooth Classic），而尼康智能设备协议的配对需要蓝牙经典。原始 ESP32 同时支持 BLE 和蓝牙经典，因此它才是目标平台。

从 PlatformIO 迁移到 [pioarduino](https://github.com/pioarduino/platform-espressif32)，因为 platformio 很糟糕，而且它停留在旧版本的 arduino-esp32，pioarduino 解决了这个问题。

代码基本完成：可以配对全新相机、与 UBlox GNSS 模块通信、解析 NMEA 并通过 BLE 发送 TIME 和 GEO 载荷。它支持多款开发板（如 M5Stack Core2 和 ESP32 WROOM 32E）；详见 `esp32/README.md`。

## 已知的相机怪癖

### LCD 坐标显示

尼康相机 LCD 以**度 + 十进制分**显示 GPS 坐标，与本项目 M5Stack 屏幕使用的格式相同。然而相机对小数分的渲染在某些情况下有 bug。

例如，`51.002'` 在相机 LCD 上显示为 `51.2'`，而不是 `51.002'`。相比之下，`51.688'` 能正确显示为 `51.688'`。这种失败并不一致——似乎取决于小数部分的数字。

写入照片 EXIF 标签的值在两种情况下都是正确的（已用 `exiftool` 验证），所以地理标记是准确的。这只是尼康相机固件的显示 bug，不是本项目编码的问题。这在 Z50II 和 Z8 上均已确认。

## 贡献

欢迎贡献！请阅读 [CONTRIBUTING.md](CONTRIBUTING.md) 了解如何构建项目、报告 bug 和提交改动。贡献者列在 [CONTRIBUTORS.md](CONTRIBUTORS.md) 中。
