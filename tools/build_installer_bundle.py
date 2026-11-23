#!/usr/bin/env python3
"""Build the first-install image: bootloader with the installer hook, the
partition table, and the resident installer, with an empty application area.
All three come from the installer project's build directory.

Flash it once over USB at 0x0. The device then boots the installer (there is no
application yet) and receives applications from the iOS app over Bluetooth.
"""

from __future__ import annotations

import argparse
import re
import shlex
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from partition_table import parse_partition_table  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
LAYOUT_HEADER = REPO / "components/installer_core/include/inst_layout.h"
BUNDLE_NAME = "Passport-Installer-FirstInstall-full.bin"
INSTALLER_BIN = "Passport-Installer.bin"
ESP32C3_CHIP_ID = 5


def read_layout(header: Path = LAYOUT_HEADER) -> dict[str, int]:
    """Read the INST_* numeric constants so Python and C cannot drift apart."""
    values: dict[str, int] = {}
    for name, value in re.findall(r"#define\s+(INST_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?\b", header.read_text()):
        values[name] = int(value, 0)
    for required in ("INST_PLAY_LIMIT", "INST_STATE_OFFSET", "INST_STATE_SIZE",
                     "INST_APP_OFFSET", "INST_APP_SIZE", "INST_WIFI_OFFSET", "INST_WIFI_SIZE",
                     "INST_FLASH_SIZE"):
        if required not in values:
            raise ValueError(f"{header}: missing {required}")
    return values


def flash_offsets(build_dir: Path) -> dict[str, int]:
    offsets: dict[str, int] = {}
    for line in (build_dir / "flash_args").read_text().splitlines():
        parts = shlex.split(line)
        if len(parts) == 2 and not parts[0].startswith("--"):
            offsets[parts[1]] = int(parts[0], 0)
    return offsets


def check_table(raw: bytes, layout: dict[str, int]) -> None:
    partitions, _ = parse_partition_table(raw)
    by_label = {p.label: p for p in partitions}
    reserved = {
        "inst_state": (0x01, 0x40, layout["INST_STATE_OFFSET"], layout["INST_STATE_SIZE"]),
        "installer": (0x00, 0x20, layout["INST_APP_OFFSET"], layout["INST_APP_SIZE"]),
        "inst_wifi": (0x01, 0x41, layout["INST_WIFI_OFFSET"], layout["INST_WIFI_SIZE"]),
    }
    for label, expected in reserved.items():
        p = by_label.get(label)
        if not p or (p.kind, p.subtype, p.offset, p.size) != expected:
            raise ValueError(f"partition table: {label} does not match inst_layout.h")
    for p in partitions:
        if p.label not in reserved and p.end > layout["INST_PLAY_LIMIT"]:
            raise ValueError(f"partition table: {p.label} reaches into the installer area")


def check_installer(image: bytes, layout: dict[str, int]) -> None:
    if len(image) < 24 or image[0] != 0xE9:
        raise ValueError("installer image: missing ESP image header")
    if int.from_bytes(image[12:14], "little") != ESP32C3_CHIP_ID:
        raise ValueError("installer image: not built for ESP32-C3")
    if len(image) > layout["INST_APP_SIZE"]:
        raise ValueError(f"installer image: {len(image)} bytes exceeds {layout['INST_APP_SIZE']}")


def build_bundle(build_dir: Path, output: Path) -> bytes:
    layout = read_layout()
    offsets = flash_offsets(build_dir)
    bootloader = (build_dir / "bootloader/bootloader.bin").read_bytes()
    table = (build_dir / "partition_table/partition-table.bin").read_bytes()
    installer = (build_dir / INSTALLER_BIN).read_bytes()
    if offsets.get("bootloader/bootloader.bin") != 0x0:
        raise ValueError("flash_args: bootloader must be at 0x0")
    if offsets.get("partition_table/partition-table.bin") != 0x8000:
        raise ValueError("flash_args: partition table must be at 0x8000")
    if bootloader[:1] != b"\xE9" or len(bootloader) > 0x8000:
        raise ValueError("bootloader image is invalid")
    if b"starting installer" not in bootloader:
        raise ValueError("bootloader does not contain the installer hook")
    check_table(table, layout)
    check_installer(installer, layout)

    bundle = bytearray(b"\xFF" * (layout["INST_APP_OFFSET"] + len(installer)))
    bundle[0:len(bootloader)] = bootloader
    bundle[0x8000:0x8000 + len(table)] = table
    bundle[layout["INST_APP_OFFSET"]:] = installer
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(bundle)
    return bytes(bundle)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True,
                        help="installer build directory (flash_args, bootloader, table, installer image)")
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        bundle = build_bundle(args.build, args.output)
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    installer_size = len(bundle) - read_layout()["INST_APP_OFFSET"]
    print(f"Installer bundle: PASS ({len(bundle)} bytes, installer {installer_size} bytes, "
          f"flash {args.output.name} at 0x0)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
