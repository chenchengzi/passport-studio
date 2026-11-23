<p align="right">
  <a href="AGENTS.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Repository Guidelines for AI Agents

This file is the mandatory entry point for AI-assisted work in Passport Studio.
Read task-specific documents from the routing table below; do not load every
document by default.

Passport Studio contains an iOS app, a resident Bluetooth installer for the
FoloToy AI Passport, libraries that let your own applications receive Wi-Fi and
settings from the phone, and the applications themselves. Board support and the
hardware, engineering, and community reference documents come from the official
[FoloToy AI Passport](https://github.com/folotoy/ai-passport) repository (MIT).

## Required AI skills

Five skills are maintained under `skills/`: `passport-develop`, `passport-setup`,
`passport-build`, `passport-device-test`, and `passport-debug`. Before
development, check that they are available in the current AI environment; if
any are missing, install them by a method suited to the tool (for example a
symlink from the tool's skills directory to `skills/<name>`) and verify that
they load. Preserve existing skills and configuration. Use only the skills that
match the task. Skills do not grant permission to flash, commit, or publish.

## Repository layout

| Path | Content |
| --- | --- |
| `ios/PassportLibrary/` | iOS app (SwiftUI, CoreBluetooth) |
| `ios/PassportKit/` | iOS protocol and logic, host-testable Swift package |
| `installer/` | Resident Bluetooth installer and bootloader hook |
| `components/bsp/` | Board support package (from upstream) |
| `components/installer_core/` | Flash layout, install journal, table planner, transfer session |
| `components/passport_wifi/` | Read Wi-Fi networks provisioned from the phone |
| `components/passport_link/` | Passport Link: settings and live data from the phone |
| `examples/hardware-demo/` | Official hardware-test demo: API reference and board self-check |
| `examples/wifi-weather/` | Own application using Wi-Fi, Passport Link, and Chinese fonts |
| `partitions.csv` | Shared table: application area below the installer's top 1 MB |
| `docs/` | Installer, Passport Link, hardware, engineering, and community reference |

## Project and safety baseline

- Target: ESP32-C3, 8 MB Flash, no PSRAM, ESP-IDF 5.5.3.
- Every application installed through the iPhone app must use the shared
  `partitions.csv` (point `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` at it) and fit
  below `INST_PLAY_LIMIT` (about 6.99 MiB). Never write to the installer area
  (`0x6FF000`–`0x800000`) or replace the bootloader; doing so removes the
  installer and requires a USB reflash of the first-install image. The layout
  contract is in [the installer design](docs/bluetooth-installer.md).
- Preserve existing user changes. Start with `git status --short --branch`;
  never overwrite or clean unrelated files.
- Flashing does not require backing up the device's current firmware, but it
  does not preserve user data either, and it does not authorize a full-chip
  erase. Follow the [flashing and data policy](docs/development/engineering/firmware-layout.md#flashing-and-stored-data).
  Flashing the first-install image replaces the device's firmware and data.
- Hardware facts follow this priority: product specification and measurement →
  `components/bsp/include/bsp_pins.h` → BSP headers and implementation →
  [hardware guide](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) →
  README and demo code. If a needed hardware detail is not defined by these
  sources, ask the user instead of guessing; never use values from another
  ESP32-C3 board.
- Reusable board logic belongs in `components/bsp`; reusable phone-device
  features in `components/passport_*`; pages, state machines, and application
  tasks in the application's own `main/`.
- New applications must design their own UI. Do not reuse the
  `examples/hardware-demo` test menu, screens, or `ui_pixel` visual shell;
  renaming or recoloring it is not a redesign. BSP APIs and non-UI logic remain
  reusable. See the [UI redesign rule](docs/development/ai-guide.md#mandatory-ui-redesign-for-derivative-applications).
- LVGL is not thread-safe. Code outside the LVGL task must hold
  `bsp_lvgl_lock()` while accessing LVGL objects.
- Before adding Chinese UI text, follow the [font checklist](docs/development/engineering/coding-conventions.md#chinese-fonts-and-missing-glyphs).
  The default Montserrat fonts have no Chinese glyphs; `examples/wifi-weather`
  shows a generated subset.
- Button callbacks must stay non-blocking; audio, storage, networking, and other
  slow work belongs in worker tasks.
- Stop every task, timer, callback, and event handler that can access a screen
  before deleting it.
- Keep state machines, protocols, timing, and layout calculations independent of
  ESP-IDF/LVGL and cover them with host tests in `tests/`, wired into
  `tools/validate.sh`.
- The installer and Passport Link protocols have C and Swift copies
  (`inst_proto.h` / `InstallerProtocol.swift`, `link_core.h` /
  `LinkProtocol.swift`). Change both sides together and bump the protocol
  version when the wire format changes.
- Never commit credentials, Wi-Fi passwords, signing team IDs, device secrets,
  private keys, personal data, or unsanitized logs. Personal signing goes only
  in the ignored `ios/PassportLibrary/Config/Signing.local.xcconfig`.
- Maintained Markdown outside the root README uses English at `<name>.md` and
  Simplified Chinese at `<name>.zh_CN.md`, with reciprocal language links.

## Adding an application

1. Create `examples/<app>/` with a `CMakeLists.txt` that adds the needed
   components from `../../components` to `EXTRA_COMPONENT_DIRS`, and a
   `sdkconfig.defaults` that uses `../../partitions.csv` (copy the target, Flash,
   and console settings from `examples/wifi-weather/sdkconfig.defaults`).
2. Use `passport_wifi` for networks provisioned by the phone and
   `passport_link` for settings and live data; see
   [Passport Link](docs/passport-link.md).
3. Add its host tests and its build, `verify_firmware.py`, and
   `archive_firmware.py --project` steps to `tools/validate.sh`.
4. Install the merged image through the iPhone app, or flash it over USB after
   approval.

## Task-specific context routing

| Task | Read before editing |
| --- | --- |
| Any firmware change | `docs/development/ai-guide.md`, relevant headers and neighboring implementation |
| Board, pins, buses, display, audio, battery, power | `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`, `components/bsp/include/bsp_pins.h`, `examples/hardware-demo/main/` |
| Installer, Flash layout, first install | `docs/bluetooth-installer.md`, `components/installer_core/include/inst_layout.h` |
| Phone settings or live data for an application | `docs/passport-link.md`, `components/passport_link/include/passport_link.h` |
| Wi-Fi | `components/passport_wifi/include/passport_wifi.h`, `docs/development/engineering/wifi-provisioning.md` |
| iOS app | `ios/PassportLibrary/App/`, `ios/PassportKit/Sources/PassportKit/` |
| Chinese UI text or fonts | `docs/development/engineering/lvgl-chinese-fonts.md`, `examples/wifi-weather/fonts/` |
| Environment or missing toolchain | `docs/development/engineering/environment-setup.md` |
| Build, test, partitions | `docs/development/engineering/build-and-test.md`, `docs/development/engineering/firmware-layout.md`, `partitions.csv` |
| Games or interactive demos | `docs/development/engineering/game-demo-to-device-acceptance.md` |
| Known pitfalls (power, keys, BLE, fonts, storage) | `docs/reference/README.md`, then only the relevant entry |

Some copied engineering documents describe the official single-application
repository (for example `main/`, `origin/demo/*` branches, or
`FoloToy-AI-Passport-full.bin`). In this repository, apply them to the
application's own directory and output names; this file and `tools/validate.sh`
take precedence.

## Required validation and delivery

```bash
./tools/validate.sh --static    # host tests
./tools/validate.sh --ios       # PassportKit tests (Xcode)
./tools/validate.sh --firmware  # installer, first-install image, examples
./tools/validate.sh             # complete gate
```

The firmware gate requires an activated ESP-IDF 5.5.3 environment. A successful
build is not hardware validation. Report these fields separately:

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: remaining board, iPhone, or user checks
```

After each completed firmware change, ask whether to flash it to the device for
testing. Detection alone is not consent; confirm the target, the exact image,
and its data impact first. See [on-device testing](docs/development/ai-guide.md#offer-on-device-testing).
Create commits and push only when the user asks.
