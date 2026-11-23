<p align="right">
  <strong>简体中文</strong> · <a href="bluetooth-installer.md">English</a>
</p>

# 蓝牙安装器与 iOS 固件库

Passport Studio 在 Flash 顶部 1 MB 常驻一个安装器。首次通过 USB 刷入后，之后换固件都由
iOS App 通过低功耗蓝牙完成：手机保存自己编译的固件和社区玩法，设备写入选中的
那一个。之后换固件不再需要电脑或 Wi-Fi。同一个连接还能为所有已安装的应用配置 Wi-Fi。

| 部分 | 位置 |
| --- | --- |
| 共享布局、日志、分区表规划、安装会话（纯 C） | `components/installer_core/` |
| 选择启动安装器还是应用的 bootloader hook | `installer/bootloader_components/installer_hook/` |
| 安装器应用（NimBLE、屏幕、Flash 写入） | `installer/` |
| 首次安装镜像打包工具 | `tools/build_installer_bundle.py` |
| iOS 逻辑（Swift 包，可在主机测试） | `ios/PassportKit/` |
| iOS App（SwiftUI、CoreBluetooth） | `ios/PassportLibrary/` |

## Flash 布局

| 范围 | 内容 | 由谁写入 |
| --- | --- | --- |
| `0x0`–`0x8000` | 带安装器 hook 的 bootloader | 仅 USB |
| `0x8000` | 分区表 | 安装器，安装的最后一步 |
| `0x9000`–`0x6FF000` | 应用区（约 6.99 MiB） | 安装器 |
| `0x6FF000` | 安装日志与记录（一个扇区） | 安装器 |
| `0x700000`–`0x7FF000` | 安装器镜像 | 仅 USB |
| `0x7FF000` | 保存的 Wi-Fi 网络（一个扇区） | 安装器 |

常量定义在 `inst_layout.h`；`partitions.csv`、打包工具和
`ios/PassportKit/.../InstallerProtocol.swift` 必须与之一致。`partitions.csv`
不一致时，打包工具测试会失败。

## 启动判断

`installer_hook.c` 在标准启动流程之前运行，满足任一条件就启动安装器：

1. 安装日志处于未完成或损坏状态（上次安装被打断）；
2. 分区表里没有 factory/ota_0 应用，或该位置没有镜像；
3. 开机时按住功能键 300 ms（三个键共用 GPIO0，都读作低电平，任意一个都可以）。
   没有按键时启动最多多出 100 ms。深度睡眠唤醒会跳过这项检查，用按键唤醒的应用
   不受影响。

如果安装器镜像本身不存在（例如用 USB 整片刷了别的合并固件），hook 会回到标准
启动流程。

## 安装流程与掉电

1. 手机发送 `BEGIN`，包含镜像末尾位置、`[0x8000, end)` 字节的 SHA-256 和显示名称。
2. 数据流的第一个扇区是镜像的分区表。安装器先校验它，并生成实际要写入的表：
   把分区裁剪到应用区内，再追加安装器条目。表被拒绝时不会擦除任何内容。
3. 先把日志标记为未完成，再紧跟写入位置往前擦除。只有整块都在镜像范围内时才用
   64 KB 块擦除，所以镜像之后的数据和 USB 刷同一个文件时一样保持不变。
4. 收到 `COMMIT` 后，安装器读回全部数据核对 SHA-256，用 ESP-IDF 校验应用镜像，
   写入并读回分区表，最后清除日志。

日志清除之前任何时刻掉电，下次都会进入安装器。连接断开时会话保留在内存里；
重新连接并发送相同的 `BEGIN` 后，从设备当前位置继续。

## 兼容规则

安装器接受标准合并固件（`idf.py merge-bin`，从 `0x0` 写入）；只有应用镜像时，
手机会套用默认布局。以下情况会被拒绝：有效数据超过 `0x6FF000`；分区表含加密、
未对齐、重叠或重名的条目；分区从安装器区域开始；数据分区跨入安装器区域且镜像
带有该分区的内容；出现第二个 `test` 应用；没有 factory 或 ota_0 应用。越界的
factory/OTA 应用分区会被裁剪，并按真实镜像长度校验。

通过这些检查不代表应用一定能正常运行。运行时会整片改写 Flash、改写自身
bootloader 或分区表的固件可能删除安装器；这时需要用 USB 重新刷入首次安装镜像。

## Wi-Fi 配网

iOS App 按优先级保存最多五个网络（密码存放在钥匙串中），并通过 `WIFI_SET` 把列表
写入安装器。安装器把它存进 `inst_wifi` 扇区（每个已安装的分区表都包含这个分区），
并在两层使用：

- **任何应用**都可以通过 `components/passport_wifi` 读取列表：
  `passport_wifi_list()` 按优先级返回网络，`passport_wifi_sta_config()` 填好一个
  `wifi_config_t`。应用应按顺序尝试，也可以保留自己的配网作为备用。
- **未修改的应用**如果使用 ESP-IDF 默认的 Flash Wi-Fi 存储，第一个网络会被写入其
  `nvs` 分区中的 station 设置（`nvs.net80211`：`sta.ssid`、`sta.pswd`；同时删除缓存
  的 PMK 和固定的 BSSID）。在已安装可启动应用时保存列表会写入一次，之后每次安装
  完成也会再写入。空列表会删除 station 凭据。安装器打不开的 NVS 分区（已满、格式
  更新、已加密或不在应用区内）会被跳过，绝不擦除。把凭据存在别处、使用
  `WIFI_STORAGE_RAM` 或调用 `esp_wifi_restore()` 的应用不受影响。

`examples/wifi-weather/` 用来验证这两层：它先按顺序尝试保存的列表，再尝试驱动设置，
在屏幕上显示用的是哪个网络、来自哪一层，并按 [Passport Link](passport-link.zh_CN.md)
推送的位置显示天气，没有推送时按公网 IP 所在位置（ipinfo.io、geojs.io 或 ipwho.is；
Open-Meteo）。固件检查会生成 `build/Wifi-Weather-merged.bin`，在 iOS App 里导入后像
其他固件一样安装。屏幕语言（英文或中文）、温度单位、刷新间隔和自动关屏都是 Passport Link 设置。中文
模式能显示固定文字和常用汉字地名；超出这些字符的 SSID 会显示为占位符。

Wi-Fi 密码以明文保存在 Flash 中，与 ESP-IDF 自己的设置相同。把设备交给别人之前，
请先在 App 里清空列表。首次安装镜像不写 `inst_wifi` 扇区，所以用 USB 重刷该镜像
会保留列表。

## 蓝牙协议

定义在 `inst_proto.h`，Swift 端有一份镜像。一个主服务包含 INFO（读）、
CONTROL（写）、DATA（无响应写）、STATUS（读、通知）和 WIFI（读；已保存的 SSID 和
上次保存结果，从不包含密码）。所有特征都要求加密且经过 MITM 认证的链路：LE Secure
Connections，配对码显示在设备屏幕上。设备不保存绑定，所以每次连接都会重新配对；
安装器只为上面所说的 Wi-Fi 设置写入应用的 NVS。DATA 携带 `offset + payload`，
全 `0xFF` 扇区用 `SKIP offset to` 跳过；顺序不对的包都会被拒绝，设备报告当前位置，
手机据此回退。手机最多保留 16 KB 未确认数据。

## 构建、测试与首次安装

```bash
./tools/validate.sh --static     # 包含安装器核心与打包工具测试
./tools/validate.sh --firmware   # 安装器、首次安装镜像与示例
cd ios/PassportKit && swift test # iOS 逻辑
```

固件检查会生成 `build/Passport-Installer-FirstInstall-full.bin`。在 `0x0` 处刷入
一次（USB 或网页刷机工具均可）。它会替换设备当前的固件和数据，开机直接进入安装器。

用免费 Apple ID 安装 iOS App：把
`ios/PassportLibrary/Config/Signing.local.xcconfig.example` 复制为
`Signing.local.xcconfig`，填入唯一的 bundle identifier 和你的 team ID，打开
`PassportLibrary.xcodeproj`，选择你的 iPhone 后运行。免费账号签名的 App 七天后
失效，从 Xcode 再运行一次即可续期。

## 验证状态

主机测试覆盖启动判断、日志、分区表规划、带跳过/丢包/续传/失败的数据流、Wi-Fi 列表
格式，以及一个真实的 7.1 MB 社区固件。`nvs.net80211` 的键名和 blob 长度取自
ESP-IDF 5.5.3 的 `libnet80211`；升级 ESP-IDF 后需要重新核对。它们不能证明掉电安全、蓝牙吞吐、iOS 配对行为或屏幕显示，
这些需要真机验收。
