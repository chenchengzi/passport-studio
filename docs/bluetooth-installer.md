<p align="right">
  <a href="bluetooth-installer.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Bluetooth Installer and iOS Library

Passport Studio keeps a resident installer in the top 1 MB of Flash. After a
one-time USB flash, firmware is installed from the iOS app over Bluetooth LE:
the phone stores your own builds and community plays, and the device writes
the selected one. No computer or Wi-Fi is needed for later changes. The same
connection also provisions Wi-Fi for every installed application.

| Part | Location |
| --- | --- |
| Shared layout, journal, table planner, install session (pure C) | `components/installer_core/` |
| Bootloader hook that selects installer or application | `installer/bootloader_components/installer_hook/` |
| Installer application (NimBLE, screen, Flash writer) | `installer/` |
| First-install image builder | `tools/build_installer_bundle.py` |
| iOS logic (Swift package, host-testable) | `ios/PassportKit/` |
| iOS app (SwiftUI, CoreBluetooth) | `ios/PassportLibrary/` |

## Flash layout

| Range | Content | Written by |
| --- | --- | --- |
| `0x0`–`0x8000` | Bootloader with the installer hook | USB only |
| `0x8000` | Partition table | Installer, last step of an install |
| `0x9000`–`0x6FF000` | Application area (about 6.99 MiB) | Installer |
| `0x6FF000` | Install journal and record (one sector) | Installer |
| `0x700000`–`0x7FF000` | Installer image | USB only |
| `0x7FF000` | Saved Wi-Fi networks (one sector) | Installer |

The constants live in `inst_layout.h`; `partitions.csv`, the bundle builder,
and `ios/PassportKit/.../InstallerProtocol.swift` must match them. The bundle
test fails when `partitions.csv` drifts.

## Boot decision

`installer_hook.c` runs before the stock boot flow and starts the installer
when any of these holds:

1. the journal is pending or torn (an install was interrupted);
2. the partition table names no factory/ota_0 application, or that slot is empty;
3. a function key is held at power-on for 300 ms (any of the three keys: they
   share GPIO0 and all read low). A released pad adds at most 100 ms to boot.
   Deep-sleep wake skips this check, so key-wake applications are unaffected.

If the installer image itself is missing (for example after a full USB flash
of another merged file), the hook falls back to the stock boot flow.

## Install sequence and power loss

1. The phone sends `BEGIN` with the image end, the SHA-256 of bytes
   `[0x8000, end)`, and a display name.
2. The first sector of the stream is the image's partition table. The
   installer validates it and builds the table it will write: partitions are
   clipped to the application area and the installer entries are appended.
   Nothing is erased if the table is rejected.
3. The journal is marked pending, then the area is erased just ahead of the
   write position. 64 KB blocks are used only when wholly inside the image, so
   data after the image is preserved exactly as with a USB flash.
4. On `COMMIT` the installer reads everything back and checks the SHA-256,
   verifies the application image with ESP-IDF, writes and reads back the
   partition table, and finally clears the journal.

A power cut at any point before the journal is cleared boots the installer
again. A disconnect keeps the session in RAM; reconnecting and sending the
same `BEGIN` resumes from the device's position.

## Application data on reinstall

Reinstalling the same application keeps its `nvs` partition: settings,
Passport Link bonds, and the Bluetooth address survive, so the phone sees the
same device. The installer keeps it only when all of these hold, and otherwise
erases it with the rest of the image:

- the installed and the incoming application have the same ESP-IDF project
  name (`project()` in the top-level `CMakeLists.txt`, stored in
  `esp_app_desc_t`); the version may differ;
- both partition tables place `nvs` at the same offset with the same size;
- the incoming image has only `0xFF` there (a merged image built by
  `idf.py merge-bin` without prebuilt NVS content);
- the last install was completed. After an interrupted install the Flash may
  hold parts of two applications, so the next install starts clean.

The kept sectors are read as `0xFF` when the written image is verified, since
those are the image's bytes. To start an application from empty data, install
a different application first. A USB flash of a merged image erases the data
as before.

## Compatibility rules

The installer accepts standard merged images (`idf.py merge-bin`, written at
`0x0`) and the phone wraps application-only images in the default layout.
It rejects images whose meaningful data passes `0x6FF000`, tables with
encrypted, misaligned, overlapping, or duplicate entries, a partition that
starts in the installer area, a data partition that crosses into it while the
image carries content for it, a second `test` application, and tables without
a factory or ota_0 application. Factory/OTA application partitions that cross
the limit are clipped and checked against the real image length.

Passing these checks does not prove an application works. Firmware that
rewrites the whole Flash, its own bootloader, or the partition table at run
time can remove the installer; reflash the first-install image over USB.

## Wi-Fi provisioning

The iOS app keeps up to five networks in priority order (passwords in the
Keychain) and writes the list to the installer with `WIFI_SET`. The installer
stores it in the `inst_wifi` sector, which every installed partition table
describes, and uses it on two layers:

- **Any application** can read the list with `components/passport_wifi`:
  `passport_wifi_list()` returns the networks in priority order, and
  `passport_wifi_sta_config()` fills a `wifi_config_t`. An application should
  try them in order and may still offer its own provisioning as a fallback.
- **Unmodified applications** that use ESP-IDF's default Flash Wi-Fi storage
  get the first network written into the station settings of their `nvs`
  partition (`nvs.net80211`: `sta.ssid`, `sta.pswd`; the cached PMK and
  pinned BSSID are removed). This happens when the list is saved while a
  bootable application is installed and again after every install. An empty
  list removes the station credentials. An NVS partition the installer cannot
  open (full, newer format, encrypted, or outside the play area) is skipped,
  never erased. Applications that keep credentials elsewhere, use
  `WIFI_STORAGE_RAM`, or call `esp_wifi_restore()` are unaffected.

`examples/wifi-weather/` exercises both layers: it tries the saved list in
order, then the driver setting, shows which network and layer it used, and
displays the weather for the location pushed over
[Passport Link](passport-link.md) or, without it, the public IP's location (ipinfo.io, geojs.io, or ipwho.is; Open-Meteo).
The firmware gate writes `build/Wifi-Weather-merged.bin`; import it in the iOS
app and install it like any other image. Its screen language (English or
Chinese), units, refresh interval, and screen timeout are Passport Link
settings. Chinese mode renders fixed text and common Chinese place names;
SSIDs outside those characters show placeholders.

Wi-Fi passwords are stored in plain text in Flash, like ESP-IDF's own
settings. Erase the list from the app before handing the device to someone
else. The first-install bundle does not write the `inst_wifi` sector, so a USB
reflash of the bundle keeps the list.

## BLE protocol

Defined in `inst_proto.h` and mirrored in Swift. One primary service with
INFO (read), CONTROL (write), DATA (write without response), STATUS
(read, notify), and WIFI (read; saved SSIDs and the last save result, never
passwords). Every characteristic requires an encrypted, MITM-authenticated
link: LE Secure Connections with a passkey shown on the device. No bonds are
stored, so each connection pairs again; the installer writes the
application's NVS only for the Wi-Fi settings described above. DATA carries `offset + payload` or `SKIP offset to` for all-`0xFF`
sectors; any out-of-order packet is rejected and the device reports its
position so the phone rewinds. The phone keeps at most 16 KB unacknowledged.

## Build, test, and first install

```bash
./tools/validate.sh --static     # includes installer core and bundle tests
./tools/validate.sh --firmware   # installer, first-install image, example
cd ios/PassportKit && swift test # iOS logic
```

The firmware gate writes `build/Passport-Installer-FirstInstall-full.bin`.
Flash it once at `0x0` (USB or the web flasher). It replaces the device's
current firmware and data and boots straight into the installer.

To install the iOS app with a free Apple ID: copy
`ios/PassportLibrary/Config/Signing.local.xcconfig.example` to
`Signing.local.xcconfig`, set a unique bundle identifier and your team ID,
open `PassportLibrary.xcodeproj`, choose your iPhone, and run. Apps signed by a
free account expire after seven days; run again from Xcode to renew.

## Verification status

Host tests cover the boot decisions, journal, table planner, streaming with
skips, loss, resume, and failures, the Wi-Fi list format, and a real 7.1 MB
community image. The `nvs.net80211` key names and blob sizes were read from
ESP-IDF 5.5.3's `libnet80211`; check them again after an ESP-IDF upgrade. They do
not prove power-loss safety, BLE throughput, iOS pairing behavior, or screen
rendering, which need device acceptance.
