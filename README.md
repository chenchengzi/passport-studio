# Passport Studio

用 iPhone 管理和配置 [FoloToy AI Passport](https://github.com/folotoy/ai-passport)：
把自己编译的固件和社区玩法存在手机上，随时通过蓝牙装到设备；给设备配置 Wi-Fi；
给正在运行的自研应用推送设置和实时数据。首次用 USB 刷入一次安装器之后，日常使用不再需要电脑。

| 部分 | 位置 |
| --- | --- |
| iOS App（SwiftUI + CoreBluetooth） | `ios/PassportLibrary/` |
| iOS 协议与逻辑（Swift Package，可在 Mac 上测试） | `ios/PassportKit/` |
| 常驻蓝牙安装器（ESP-IDF 工程，含 bootloader hook） | `installer/` |
| 安装器核心：布局、日志、分区表规划、传输会话 | `components/installer_core/` |
| 自研应用读取手机配置的 Wi-Fi | `components/passport_wifi/` |
| 自研应用与手机的蓝牙通道 Passport Link | `components/passport_link/` |
| 示例应用：Wi-Fi 天气（Wi-Fi + Passport Link） | `examples/wifi-weather/` |
| 官方硬件测试演示（硬件 API 参考、板子自检） | `examples/hardware-demo/` |
| 板级支持（来自 FoloToy ai-passport） | `components/bsp/` |

## 功能

- **固件库与蓝牙安装**：在 App 里点 **+**，导入本地 `.bin`（隔空投送或「文件」），或粘贴
  社区玩法链接，例如 `https://ai-passport.folotoy.cn/plays/?play=pocket-tetris`。设备关机后
  **按住上键再开机**进入安装模式，在 App 里选择固件 → 安装到设备，输入设备屏幕上的 6 位配对码。
  安装模式下长按 OK（松开后）可直接启动已安装的固件。
- **Wi-Fi 配网**：按优先级保存最多 5 个网络并同步到设备。自研固件用 `passport_wifi` 读取；
  使用 ESP-IDF 默认 Wi-Fi 存储的固件会自动拿到第一个网络。设备上的密码以明文保存在 Flash 中。
- **设备连接（Passport Link）**：运行中的自研应用通过蓝牙声明自己的设置项和需要的数据频道
  （位置、时间等），App 自动生成设置页并推送内容，首次配对后自动重连。

## 构建

需要 ESP-IDF 5.5.3（ESP32-C3、8 MB Flash）和 Xcode。

```bash
./tools/validate.sh --static     # 主机测试：BSP、硬件演示、安装器核心、Passport Link、天气逻辑、固件工具
./tools/validate.sh --ios        # PassportKit 测试（需要 Xcode）
./tools/validate.sh --firmware   # 安装器、首次安装镜像、两个示例（需要激活 ESP-IDF）
```

1. **首次安装**：把 `build/Passport-Installer-FirstInstall-full.bin` 从 `0x0` 刷入设备（USB 或
   [网页刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)）。**这会替换设备当前的固件和数据。**
   开机后屏幕显示 **INSTALLER**。
2. **iOS App**：把 `ios/PassportLibrary/Config/Signing.local.xcconfig.example` 复制为
   `Signing.local.xcconfig`，填入自己唯一的 bundle identifier 和 team ID，用 Xcode 打开
   `ios/PassportLibrary/PassportLibrary.xcodeproj` 运行到 iPhone。免费 Apple ID 签名七天后需重新运行。
3. **试一下**：`build/Wifi-Weather-merged.bin` 通过 AirDrop 发到手机，导入后安装。

## 限制

- 固件有效数据必须小于约 6.99 MiB（安装器占用 Flash 顶部 1 MB）。App 会在安装前标出不兼容的固件和原因。
- 运行时整片改写 Flash、或自带 bootloader / 分区表的固件可能覆盖安装器，这时需要用 USB 重新刷入首次安装镜像。
- 安装器界面为英文。

## 文档

- [蓝牙安装器与 iOS 固件库](docs/bluetooth-installer.zh_CN.md)（[English](docs/bluetooth-installer.md)）：
  Flash 布局、启动判断、掉电处理、兼容规则和蓝牙协议。
- [Passport Link](docs/passport-link.zh_CN.md)（[English](docs/passport-link.md)）：自研应用与手机之间的蓝牙通道。
- [硬件开发指南](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)、[规格](docs/hardware-design/specifications.zh_CN.md)：
  引脚、屏幕、音频、电量计、按键、功耗。
- [AI 开发指南](docs/development/ai-guide.zh_CN.md) 与 [工程文档](docs/development/engineering/)：环境搭建、构建测试、
  编码规范、固件布局、中文字体、Wi-Fi。
- [社区经验库](docs/reference/README.zh_CN.md)：其他开发者在真机上踩过的坑。

## 用 AI 开发自己的应用

AI 从 [`AGENTS.zh_CN.md`](AGENTS.zh_CN.md)（[English](AGENTS.md)）开始：板子参数、安全规则、新应用怎么接入
安装器和 Passport Link、按任务读哪些文档，以及验证和交付格式。`skills/` 下有开发、环境搭建、构建、
上板测试、调试五个 skill。

## 许可

MIT。`components/bsp` 等部分源自 [FoloToy AI Passport](https://github.com/folotoy/ai-passport)（MIT），
保留其版权声明。本项目不是 FoloToy 官方项目。
