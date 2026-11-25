<p align="right">
  <strong>简体中文</strong> · <a href="passport-link.md">English</a>
</p>

# Passport Link：手机到应用的蓝牙通道

Passport Link 让正在运行的应用通过低功耗蓝牙从 iOS App 接收设置和实时内容。
它是[蓝牙安装器](bluetooth-installer.zh_CN.md)的补充：

| | 安装器 | Passport Link |
| --- | --- | --- |
| 何时 | 安装模式（按住上键开机） | 应用运行时 |
| 范围 | 所有应用共享：固件、Wi-Fi 列表 | 单个应用：它的设置和内容 |
| 存储 | Flash 顶部的安装器扇区 | 应用自己的 NVS |
| 配对 | 每次连接都配对，不绑定 | 配对一次，之后绑定 |

| 部分 | 位置 |
| --- | --- |
| 设备端组件（NimBLE 外设、设置存储） | `components/passport_link/` |
| 纯 C 分帧与设置校验（主机测试） | `components/passport_link/link_core.c` |
| iOS 协议（Swift 包，主机测试） | `ios/PassportKit/Sources/PassportKit/LinkProtocol.swift` |
| iOS 连接、数据来源、自动生成的设置页 | `ios/PassportLibrary/App/Link*.swift` |
| 示例 | `examples/wifi-weather/` |

## 目标与限制

- 应用声明自己接收的频道和一份设置表。手机根据设置表生成设置页；新应用不需要
  修改 iOS App。
- 手机在连接时、数据变化时、设备请求时推送内容（第 1 版：`location` 和 `time`）。
- 消息是小 JSON 文档，最大 4 KB。低功耗蓝牙速度约 10–50 KB/s；Link 不适合传
  图片或音频。
- App 在前台或挂起到后台时（`bluetooth-central` 模式），iOS 会保持连接。第 1 版
  只在使用 App 期间读取定位，所以打开 App 时才更新位置。
- ESP32-C3 的 Wi-Fi 和蓝牙共用一个射频。两者都用的应用应控制 Link 的流量，并预期
  Wi-Fi 吞吐会下降。

## GATT 服务

服务 `CDDE7C00-DD5A-47C6-890F-268DE0F6918A`，广播时使用应用的显示名称。所有特征
都要求加密且经过 MITM 认证的链路。

| UUID 后缀 | 名称 | 属性 | 内容 |
| --- | --- | --- | --- |
| `7C01` | INFO | 读 | 应用描述，JSON，最多 512 字节 |
| `7C02` | RX | 写、无响应写 | 手机发来的帧 |
| `7C03` | TX | 通知 | 发往手机的帧 |

### 分帧

每次 RX 写入和 TX 通知都是一帧：`u8 flags` 后跟消息的一部分。bit 0 表示消息的
第一帧，bit 1 表示最后一帧。单帧消息的 flags 为 `0x03`。收到新的第一帧会丢弃
未完成的消息。每帧最多携带 ATT MTU − 4 字节；重组后超过 4096 字节的消息会被丢弃。

### INFO

```json
{"v":1,"id":"wifi-weather","name":"Passport Weather","ver":"1.1.0",
 "ch":["location","time"],
 "set":[{"k":"units","n":"Units","type":"enum","opt":["C","F"],"def":"C"},
        {"k":"refresh","n":"Refresh (min)","type":"int","min":5,"max":180,"def":30}]}
```

`id` 标识应用，手机按应用分别保存设置。设置类型有 `bool`、`int`（`min`、`max`）、
`enum`（`opt`）和 `text`（`max` 字节）。键名最多 15 个字符（NVS 键的限制）。标签
`n` 只在手机上显示，可以使用任何语言。

整个 INFO 不能超过 512 字节，一个汉字占 3 个字节。超出时 `passport_link_start()` 会失败，
设备完全不广播 Link。`tools/check_link_info.py`（包含在 `./tools/validate.sh --static`
中）会按源码重建每个示例的 INFO，超长时检查失败；标签要尽量简短，或合并设置项。

### 消息

每条消息都是带类型字段 `t` 的 JSON 对象。

| 方向 | `t` | 字段 | 含义 |
| --- | --- | --- | --- |
| 手机 → 设备 | `get` | | 请求当前设置 |
| 手机 → 设备 | `set` | `v`：键到值的对象 | 修改设置，可以只改一部分 |
| 手机 → 设备 | `push` | `ch`、`d` | 某个频道的内容 |
| 设备 → 手机 | `settings` | `v`：全部当前值 | 对 `get` 和 `set` 的回复 |
| 设备 → 手机 | `err` | `why`，可选 `k` | 被拒绝的消息或设置 |
| 设备 → 手机 | `req` | `ch` | 请手机立即推送某个频道 |

只有所有值都通过设置表校验时，`set` 才会生效；否则什么都不改，设备回复 `err`
并指出出错的键。

### 第 1 版的频道

| 频道 | `d` | 设备端处理 |
| --- | --- | --- |
| `time` | `{"unix": 1763640000, "tz": 480}` | 组件设置系统时钟；`tz` 是相对 UTC 向东的分钟数 |
| `location` | `{"lat": 39.98, "lon": 116.31, "acc": 35, "name": "Haidian, Beijing"}` | 交给应用；`name` 是英文地名 |

应用声明的其他频道会原样交给应用，所以新增频道只需要在手机端加数据来源。

## 安全与绑定

Link 使用 LE Secure Connections，配对码由应用显示（`on_passkey` 回调），绑定信息
保存在应用的 NVS 中，之后的连接自动完成。设备使用保存在同一 NVS 命名空间中的静态
随机地址。通过安装器重新安装同一个应用时 NVS 分区会保留（见
[安装器](bluetooth-installer.zh_CN.md#重装时的应用数据)），地址和绑定信息都不变，手机会
自动重连。安装擦除 NVS 分区时，地址随绑定信息一起变化，所以 iOS 会把设备当作新设备，
而不是因为过期的绑定而失败。如果应用自己擦除了 NVS，需要先在
iOS「设置 › 蓝牙」中忽略该设备再重新配对。

## 设备端 API

```c
#include "passport_link.h"

static const passport_link_setting_t SETTINGS[] = {
    PASSPORT_LINK_ENUM("units", "Units", "C", "C", "F"),
    PASSPORT_LINK_INT("refresh", "Refresh (min)", 30, 5, 180),
};
static const char *const CHANNELS[] = {"location", "time"};

passport_link_config_t cfg = {
    .app_id = "wifi-weather", .display_name = "Passport Weather",
    .channels = CHANNELS, .channel_count = 2,
    .settings = SETTINGS, .setting_count = 2,
    .on_push = on_push,           // (channel, cJSON *data, user)
    .on_settings = on_settings,   // 设置已改变
    .on_passkey = on_passkey,     // 显示 / 清除（0）配对码
    .on_connection = on_connection,
};
passport_link_start(&cfg);
int32_t refresh = passport_link_get_int("refresh");
```

调用 `passport_link_start()` 前，`nvs_flash_init()` 必须已经成功。回调运行在组件
自己的任务里，不会运行在 NimBLE host 任务上；回调可以短暂阻塞，但访问屏幕前必须
持有 LVGL 锁。`passport_link_request(channel)` 请已连接的手机推送某个频道。

## iOS App

**设备连接**页列出附近的 Link 应用，完成配对并记住设备；之后只要记住的设备在
范围内，iOS 就会自动重连。对每个已连接的应用，页面显示它的描述、根据设置表生成的
设置表单，以及它用到的数据来源。连接后 App 会发送 `get` 和当前时间；应用声明了
`location` 频道时再发送位置。定位使用「使用 App 期间」权限，并用英文做反向地理编码。

## 示例：Wi-Fi 天气

天气示例声明了 `location` 和 `time` 频道，以及 `units` 和 `refresh` 两项设置。
手机推送的位置会替代按 IP 估计的位置，并保存在 NVS 中，所以不连手机重启后也有效。
修改设置会立即刷新屏幕。

## 验证

两端的主机测试覆盖分帧、重组长度限制和设置表校验。配对、跨重连和重装的绑定、
与 Wi-Fi 共存，以及 iOS 后台重连，需要真机验收。
