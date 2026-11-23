<p align="right">
  <a href="passport-link.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Passport Link: Phone-to-Application Bluetooth Channel

Passport Link lets a running application receive settings and live content
from the iOS app over Bluetooth LE. It complements the
[Bluetooth installer](bluetooth-installer.md):

| | Installer | Passport Link |
| --- | --- | --- |
| When | Installer mode (hold UP at power-on) | While the application runs |
| Scope | Shared by every application: firmware, Wi-Fi list | One application: its settings and content |
| Storage | Installer sectors at the top of Flash | The application's own NVS |
| Pairing | Every connection, no bond | Once, then bonded |

| Part | Location |
| --- | --- |
| Device component (NimBLE peripheral, settings store) | `components/passport_link/` |
| Pure framing and setting validation (host-tested) | `components/passport_link/link_core.c` |
| iOS protocol (Swift package, host-tested) | `ios/PassportKit/Sources/PassportKit/LinkProtocol.swift` |
| iOS connection, providers, generated settings | `ios/PassportLibrary/App/Link*.swift` |
| Example | `examples/wifi-weather/` |

## Goals and limits

- An application declares channels it accepts and a settings schema. The phone
  generates the settings page from the schema; no iOS change is needed for a
  new application.
- The phone pushes content (version 1: `location` and `time`) when it
  connects, when the data changes, and when the device asks.
- Messages are small JSON documents, at most 4 KB. Bluetooth LE moves roughly
  10–50 KB/s; Link is not meant for images or audio.
- iOS keeps the connection while the app runs or is suspended in the
  background (`bluetooth-central` mode). Version 1 reads location only while
  the app is in use, so location updates arrive when the app is opened.
- The ESP32-C3 shares one radio between Wi-Fi and Bluetooth. Applications that
  use both should keep Link traffic light and expect lower Wi-Fi throughput.

## GATT service

Service `CDDE7C00-DD5A-47C6-890F-268DE0F6918A`, advertised with the
application's display name. Every characteristic requires an encrypted,
MITM-authenticated link.

| UUID suffix | Name | Properties | Content |
| --- | --- | --- | --- |
| `7C01` | INFO | read | Application description, JSON, at most 512 bytes |
| `7C02` | RX | write, write without response | Frames from the phone |
| `7C03` | TX | notify | Frames to the phone |

### Framing

Each RX write and TX notification is one frame: `u8 flags` followed by part of
a message. Bit 0 marks the first frame of a message, bit 1 the last. A
single-frame message has flags `0x03`. A first frame discards any partial
message. Frames carry at most ATT MTU − 4 bytes of message; a reassembled
message larger than 4096 bytes is dropped.

### INFO

```json
{"v":1,"id":"wifi-weather","name":"Passport Weather","ver":"1.1.0",
 "ch":["location","time"],
 "set":[{"k":"units","n":"Units","type":"enum","opt":["C","F"],"def":"C"},
        {"k":"refresh","n":"Refresh (min)","type":"int","min":5,"max":180,"def":30}]}
```

`id` identifies the application, so the phone keeps settings per application.
Setting types are `bool`, `int` (`min`, `max`), `enum` (`opt`), and `text`
(`max` bytes). Keys have at most 15 characters (an NVS key). Labels (`n`) are
shown only on the phone, so they may use any language.

### Messages

Every message is a JSON object with a type `t`.

| Direction | `t` | Fields | Meaning |
| --- | --- | --- | --- |
| phone → device | `get` | | Request current settings |
| phone → device | `set` | `v`: object of keys to values | Change settings; partial updates allowed |
| phone → device | `push` | `ch`, `d` | Content for a channel |
| device → phone | `settings` | `v`: all current values | Reply to `get` and `set` |
| device → phone | `err` | `why`, optional `k` | A rejected message or setting |
| device → phone | `req` | `ch` | Ask the phone to push a channel now |

A `set` is applied only when every value passes the schema; otherwise nothing
changes and the device answers `err` with the offending key.

### Channels in version 1

| Channel | `d` | Device handling |
| --- | --- | --- |
| `time` | `{"unix": 1763640000, "tz": 480}` | The component sets the system clock; `tz` is minutes east of UTC |
| `location` | `{"lat": 39.98, "lon": 116.31, "acc": 35, "name": "Haidian, Beijing"}` | Passed to the application; `name` is an English place name |

Applications receive every other declared channel unchanged, so new channels
need only a phone-side provider.

## Security and bonding

Link uses LE Secure Connections with a passkey shown by the application
(`on_passkey` callback) and stores the bond in the application's NVS, so later
connections are automatic. The device uses a static random address stored in
the same NVS namespace. When an install through the installer erases the NVS
partition, the address changes with the bonds, so iOS treats the device as
new instead of failing with a stale bond. If an application erases its NVS by
itself, remove the device in iOS Settings › Bluetooth before pairing again.

## Device API

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
    .on_settings = on_settings,   // values changed
    .on_passkey = on_passkey,     // show / clear (0) the pairing code
    .on_connection = on_connection,
};
passport_link_start(&cfg);
int32_t refresh = passport_link_get_int("refresh");
```

`passport_link_start()` expects `nvs_flash_init()` to have succeeded.
Callbacks run on the component's own task, never on the NimBLE host task; they
may block briefly but must take the LVGL lock before touching the screen.
`passport_link_request(channel)` asks a connected phone to push a channel.

## iOS app

The **Device Link** page lists nearby Link applications, pairs, and remembers
devices; iOS then reconnects automatically whenever a remembered device is in
range. For each connected application it shows the description, a settings
form generated from the schema, and the providers it uses. On connect the app
sends `get`, the current time, and the location when the application lists
the `location` channel.
Location uses When-In-Use permission and reverse geocoding in English.

## Example: Wi-Fi weather

The weather example declares `location` and `time`, and the `units` and
`refresh` settings. A pushed location replaces the IP-based guess and is kept
in NVS, so it survives restarts without the phone. Changing a setting
refreshes the screen immediately.

## Verification

Host tests cover framing, reassembly limits, and schema validation on both
sides. Pairing, bonding across reconnects and reinstalls, Wi-Fi coexistence,
and iOS background reconnection need device acceptance.
