<p align="right">
  <strong>简体中文</strong> · <a href="AGENTS.md">English</a>
</p>

# AI Agent 仓库规范

本文件是在 Passport Studio 中进行 AI 辅助开发的必读入口。按下方路由表阅读与任务相关的文档，
不要默认加载全部文档。

Passport Studio 包含 iOS App、FoloToy AI Passport 的常驻蓝牙安装器、让自研应用从手机获取
Wi-Fi 和设置的组件库，以及应用本身。板级支持和硬件、工程、社区参考文档来自官方
[FoloToy AI Passport](https://github.com/folotoy/ai-passport) 仓库（MIT）。

## 必需的 AI skills

`skills/` 下维护五个 skill：`passport-develop`、`passport-setup`、`passport-build`、
`passport-device-test` 和 `passport-debug`。开发前检查它们在当前 AI 环境中是否可用；
缺少时按所用工具选择合适方式自行安装（例如在工具的 skills 目录建立指向 `skills/<name>`
的符号链接），并确认能被加载。保留已有 skill 和配置。只使用与任务匹配的 skill。
skill 不授予烧录、提交或发布的权限。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `ios/PassportLibrary/` | iOS App（SwiftUI、CoreBluetooth） |
| `ios/PassportKit/` | iOS 协议与逻辑，可在主机上测试的 Swift 包 |
| `installer/` | 常驻蓝牙安装器和 bootloader hook |
| `components/bsp/` | 板级支持包（来自上游） |
| `components/installer_core/` | Flash 布局、安装日志、分区表规划、传输会话 |
| `components/passport_wifi/` | 读取手机配置的 Wi-Fi 网络 |
| `components/passport_link/` | Passport Link：从手机接收设置和实时数据 |
| `examples/hardware-demo/` | 官方硬件测试演示：API 参考和板子自检 |
| `examples/wifi-weather/` | 使用 Wi-Fi、Passport Link 和中文字体的自研应用 |
| `partitions.csv` | 共用分区表：应用区位于安装器占用的顶部 1 MB 之下 |
| `docs/` | 安装器、Passport Link、硬件、工程和社区参考文档 |

## 项目与安全基线

- 目标：ESP32-C3、8 MB Flash、无 PSRAM、ESP-IDF 5.5.3。
- 通过 iPhone App 安装的每个应用都必须使用共用的 `partitions.csv`
  （`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` 指向它），并且不超过 `INST_PLAY_LIMIT`
  （约 6.99 MiB）。不得写入安装器区域（`0x6FF000`–`0x800000`）或替换 bootloader；
  否则会删除安装器，需要用 USB 重新刷入首次安装镜像。布局约定见
  [安装器设计](docs/bluetooth-installer.zh_CN.md)。
- 保留用户已有改动。先运行 `git status --short --branch`；不要覆盖或清理无关文件。
- 烧录前不需要备份设备上的现有固件，但烧录也不保证保留用户数据，且不代表允许整片擦除。
  遵循[烧录与数据策略](docs/development/engineering/firmware-layout.zh_CN.md#烧录与已存数据)。
  刷入首次安装镜像会替换设备的固件和数据。
- 硬件事实按以下优先级：产品规格与实测 → `components/bsp/include/bsp_pins.h` →
  BSP 头文件与实现 → [硬件指南](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md) →
  README 与演示代码。需要的硬件细节在这些来源中没有定义时，询问用户，不要猜测；
  不得使用其他 ESP32-C3 板子的参数。
- 可复用的板级逻辑放在 `components/bsp`；可复用的手机-设备功能放在 `components/passport_*`；
  页面、状态机和应用任务放在应用自己的 `main/`。
- 新应用必须自行设计 UI。不得复用 `examples/hardware-demo` 的测试菜单、页面或 `ui_pixel`
  视觉外壳；改名或换色不算重新设计。BSP API 和非 UI 逻辑可以复用。见
  [UI 重新设计规则](docs/development/ai-guide.zh_CN.md)。
- LVGL 不是线程安全的。LVGL 任务之外的代码访问 LVGL 对象时必须持有 `bsp_lvgl_lock()`。
- 添加中文界面文字前，按[字体检查清单](docs/development/engineering/coding-conventions.zh_CN.md)
  处理。默认 Montserrat 字体没有中文字形；`examples/wifi-weather` 演示了生成的字体子集。
- 按键回调必须保持非阻塞；音频、存储、网络等慢操作放在工作任务中。
- 删除页面前，先停止所有可能访问该页面的任务、定时器、回调和事件处理器。
- 状态机、协议、时序和布局计算与 ESP-IDF/LVGL 解耦，并在 `tests/` 中写主机测试，
  接入 `tools/validate.sh`。
- 安装器和 Passport Link 协议各有 C 与 Swift 两份（`inst_proto.h` / `InstallerProtocol.swift`、
  `link_core.h` / `LinkProtocol.swift`）。两端同时修改；线上格式变化时提升协议版本。
- 不得提交凭证、Wi-Fi 密码、签名 team ID、设备密钥、私钥、个人数据或未脱敏日志。
  个人签名只放在被忽略的 `ios/PassportLibrary/Config/Signing.local.xcconfig`。
- 根 README 以外的维护文档：英文放在 `<name>.md`，简体中文放在 `<name>.zh_CN.md`，
  互相保留语言链接。

## 新增应用

1. 新建 `examples/<app>/`：`CMakeLists.txt` 把需要的 `../../components` 组件加入
   `EXTRA_COMPONENT_DIRS`；`sdkconfig.defaults` 使用 `../../partitions.csv`
   （目标芯片、Flash 和控制台设置照抄 `examples/wifi-weather/sdkconfig.defaults`）。
2. 用 `passport_wifi` 读取手机配置的网络，用 `passport_link` 接收设置和实时数据；见
   [Passport Link](docs/passport-link.zh_CN.md)。
3. 把它的主机测试，以及构建、`verify_firmware.py`、`archive_firmware.py --project`
   三步加入 `tools/validate.sh`。
4. 通过 iPhone App 安装合并镜像，或在获得同意后用 USB 烧录。

## 按任务读取上下文

| 任务 | 修改前阅读 |
| --- | --- |
| 任意固件修改 | `docs/development/ai-guide.zh_CN.md`、相关头文件和相邻实现 |
| 板子、引脚、总线、屏幕、音频、电池、功耗 | `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md`、`components/bsp/include/bsp_pins.h`、`examples/hardware-demo/main/` |
| 安装器、Flash 布局、首次安装 | `docs/bluetooth-installer.zh_CN.md`、`components/installer_core/include/inst_layout.h` |
| 应用从手机获取设置或实时数据 | `docs/passport-link.zh_CN.md`、`components/passport_link/include/passport_link.h` |
| Wi-Fi | `components/passport_wifi/include/passport_wifi.h`、`docs/development/engineering/wifi-provisioning.zh_CN.md` |
| iOS App | `ios/PassportLibrary/App/`、`ios/PassportKit/Sources/PassportKit/` |
| 中文界面文字或字体 | `docs/development/engineering/lvgl-chinese-fonts.zh_CN.md`、`examples/wifi-weather/fonts/` |
| 开发环境或缺少工具链 | `docs/development/engineering/environment-setup.zh_CN.md` |
| 构建、测试、分区 | `docs/development/engineering/build-and-test.zh_CN.md`、`docs/development/engineering/firmware-layout.zh_CN.md`、`partitions.csv` |
| 游戏或交互演示 | `docs/development/engineering/game-demo-to-device-acceptance.zh_CN.md` |
| 已知坑（功耗、按键、BLE、字体、存储） | `docs/reference/README.zh_CN.md`，再只读相关条目 |

部分搬来的工程文档描述的是官方单应用仓库（例如 `main/`、`origin/demo/*` 分支或
`FoloToy-AI-Passport-full.bin`）。在本仓库中，把它们对应到应用自己的目录和产物名；
本文件和 `tools/validate.sh` 优先。

## 验证与交付

```bash
./tools/validate.sh --static    # 主机测试
./tools/validate.sh --ios       # PassportKit 测试（需要 Xcode）
./tools/validate.sh --firmware  # 安装器、首次安装镜像、示例
./tools/validate.sh             # 完整门禁
```

固件门禁需要先激活 ESP-IDF 5.5.3。构建成功不等于硬件验证。分别报告：

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: 剩余的板子、iPhone 或用户检查
```

每完成一次固件修改，主动询问是否烧录到设备测试。检测到设备不等于同意；先确认目标设备、
具体镜像及其数据影响。见[上板测试](docs/development/ai-guide.zh_CN.md)。
只有用户要求时才提交和推送。
