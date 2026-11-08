# nsg 安卓遥控端

[English](README.md) | 中文

这个安卓 App 把一台闲置手机变成尼康 Z 相机的 GPS 源。手机通过蓝牙 smart-device 模式连接相机，持续把位置写进相机，拍出来的照片就会带地理坐标。

它是 [hurui200320/nsg](https://github.com/hurui200320/nsg) 的安卓实现。SnapBridge 的配对协议来自 [gkoh/furble](https://github.com/gkoh/furble) 的逆向成果，设备标识恢复算法单独放在 [snapbridge-id-extractor](https://github.com/HowenXu/snapbridge-id-extractor)。

## 现在的情况

App 已经能正常使用，当前版本是 1.0.1。

相机实测过尼康 Z7 II。理论上，所有支持 SnapBridge 蓝牙 smart-device 模式的 Z 相机都可以用。目前只做了蓝牙模式，所以主要覆盖 Z 卡口微单和部分带蓝牙的末代单反。

手机实测过华为 Mate 40 Pro 和华为 MatePad Pro 10.8。最低支持 Android 7.0（API 24）。

## 功能

- 搜索并配对新的相机。
- 连接已保存相机，可以设置启动时默认连接的那一台。
- 从相机已有的 SnapBridge 配对记录里恢复设备标识，不需要先删掉 SnapBridge 的配对。
- 把 GPS 和网络定位写入相机。
- 前台服务保活，后台继续发送位置，相机重新出现时自动重连。
- 主界面带日志区，能看到连接和标识恢复的进度。
- 跟随系统语言：中文系统显示中文，其它语言显示英文。

## 安装

从 [Releases](https://github.com/HowenXu/nsg/releases) 下载 `nsg.apk` 安装。

第一次打开需要给这些权限：

- 位置权限，用来获取 GPS。
- 蓝牙权限，用来连接相机。
- 通知权限，前台服务需要。
- 电池优化豁免。国产 ROM 还要把 App 加进自启动和后台白名单。

## 配对相机

### 相机已经和 SnapBridge 配对过

这是最常见的情况。

1. 打开 App，点「配对新相机」。
2. 在列表里选中相机。
3. 如果相机已有 SnapBridge 配对记录，App 会自动恢复设备标识，日志区会显示进度。
4. 完成后自动连接，状态变成「就绪」。

之后日常使用点「连接已保存相机」即可。切回 SnapBridge 也不需要删除任何配对记录。

### 全新相机

点「配对新相机」，选中相机，然后在系统弹窗里确认蓝牙配对。

## 使用

状态变成「就绪」后，相机 LCD 会显示 GPS 图标或坐标。这时拍的照片，EXIF 里会带位置信息。

连接后可以直接把 App 放到后台。前台服务会保持连接，并继续以较低频率发送位置。

## 高级设置

右上角菜单里进「高级设置」，一般不需要改。

- **相机端设备名**：相机上显示的控制器名称。默认会按 SnapBridge 的格式自动生成。
- **固定设备标识**：完整的 16 位十六进制 SnapBridge 设备标识。自动恢复成功后会自动填入。

## 已知问题

- 不要清除 SnapBridge 的数据，也不要重装它。它会重新生成设备标识，需要重新跑一次恢复。
- 尼康相机 LCD 有时会把小数分钟显示错，例如 `51.002'` 显示成 `51.2'`。照片 EXIF 里的坐标是正确的。
- 目前只支持蓝牙 smart-device 模式，只走 Wi-Fi 的机型不在范围内。
- 部分国产 ROM 会积极杀后台。电池优化和自启动白名单比 App 本身的设置更重要。

## 构建

本地 release 默认不签名，干净仓库不需要 keystore 也能直接构建：

```bash
cd android
./gradlew assembleRelease
```

产物是 `app/build/outputs/apk/release/app-release-unsigned.apk`。

推送 `v*` tag 时，release 工作流会构建、签名、校验并上传签名好的 `nsg.apk`。工作流需要这些仓库 secrets：

| Secret | 含义 |
| --- | --- |
| `KEYSTORE_BASE64` | release keystore 的 Base64 |
| `KEYSTORE_PASSWORD` | keystore 密码 |
| `KEY_ALIAS` | key 别名 |
| `KEY_PASSWORD` | key 密码 |

项目整体协议以仓库根目录为准。贡献者名单见 [CONTRIBUTORS.md](../CONTRIBUTORS.md)。
