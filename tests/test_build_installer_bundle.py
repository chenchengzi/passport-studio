#!/usr/bin/env python3
"""Host tests for the first-install bundle builder."""

from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "build_installer_bundle", ROOT / "tools" / "build_installer_bundle.py"
)
assert SPEC and SPEC.loader
BUNDLE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = BUNDLE
SPEC.loader.exec_module(BUNDLE)

LAYOUT = BUNDLE.read_layout()
ENTRY = struct.Struct("<HBBII16sI")


def table(entries) -> bytes:
    raw = bytearray(b"\xff" * 0xC00)
    for index, (kind, subtype, offset, size, label) in enumerate(entries):
        ENTRY.pack_into(raw, index * 32, 0x50AA, kind, subtype, offset, size,
                        label.encode().ljust(16, b"\0"), 0)
    end = len(entries) * 32
    raw[end:end + 32] = b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(raw[:end]).digest()
    return bytes(raw)


def repo_table() -> bytes:
    return table((
        (1, 2, 0x9000, 0x6000, "nvs"),
        (1, 1, 0xF000, 0x1000, "phy_init"),
        (0, 0, 0x10000, LAYOUT["INST_PLAY_LIMIT"] - 0x10000, "factory"),
        (1, 0x40, LAYOUT["INST_STATE_OFFSET"], LAYOUT["INST_STATE_SIZE"], "inst_state"),
        (0, 0x20, LAYOUT["INST_APP_OFFSET"], LAYOUT["INST_APP_SIZE"], "installer"),
        (1, 0x41, LAYOUT["INST_WIFI_OFFSET"], LAYOUT["INST_WIFI_SIZE"], "inst_wifi"),
    ))


def esp_image(size: int, chip: int = 5) -> bytes:
    header = bytearray(24)
    header[0] = 0xE9
    header[12:14] = chip.to_bytes(2, "little")
    return bytes(header) + bytes((i * 7) & 0xFF for i in range(size - 24))


class BundleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "bootloader").mkdir()
        (self.dir / "partition_table").mkdir()
        (self.dir / "flash_args").write_text(
            "--flash_mode dio --flash_freq 80m --flash_size 8MB\n"
            "0x0 bootloader/bootloader.bin\n"
            "0x10000 Passport-Installer.bin\n"
            "0x8000 partition_table/partition-table.bin\n")
        self.write(b"\xE9" + b"\0" * 64 + b"starting installer" + b"\0" * 64, repo_table())
        self.installer = self.dir / BUNDLE.INSTALLER_BIN
        self.installer.write_bytes(esp_image(0x40000))
        self.output = self.dir / "out" / BUNDLE.BUNDLE_NAME

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write(self, bootloader: bytes, partition_table: bytes) -> None:
        (self.dir / "bootloader/bootloader.bin").write_bytes(bootloader)
        (self.dir / "partition_table/partition-table.bin").write_bytes(partition_table)

    def build(self) -> bytes:
        return BUNDLE.build_bundle(self.dir, self.output)

    def test_layout(self) -> None:
        bundle = self.build()
        installer = self.installer.read_bytes()
        app = LAYOUT["INST_APP_OFFSET"]
        self.assertEqual(len(bundle), app + len(installer))
        self.assertEqual(bundle[0], 0xE9)
        self.assertEqual(bundle[0x8000:0x8C00], repo_table())
        self.assertEqual(bundle[app:], installer)
        # Application area and journal are erased so the first boot opens the installer.
        self.assertEqual(set(bundle[0x10000:app]), {0xFF})
        self.assertEqual(self.output.read_bytes(), bundle)

    def test_rejects_missing_hook(self) -> None:
        self.write(b"\xE9" + b"\0" * 200, repo_table())
        with self.assertRaisesRegex(ValueError, "hook"):
            self.build()

    def test_rejects_table_without_installer(self) -> None:
        self.write(b"\xE9starting installer", table((
            (1, 2, 0x9000, 0x6000, "nvs"),
            (0, 0, 0x10000, 0x7F0000, "factory"),
        )))
        with self.assertRaisesRegex(ValueError, "inst_state"):
            self.build()

    def test_rejects_partition_in_installer_area(self) -> None:
        self.write(b"\xE9starting installer", table((
            (0, 0, 0x10000, LAYOUT["INST_PLAY_LIMIT"] - 0x10000 + 0x1000, "factory"),
            (1, 0x40, LAYOUT["INST_STATE_OFFSET"] + 0x1000, LAYOUT["INST_STATE_SIZE"], "inst_state"),
            (0, 0x20, LAYOUT["INST_APP_OFFSET"], LAYOUT["INST_APP_SIZE"], "installer"),
            (1, 0x41, LAYOUT["INST_WIFI_OFFSET"], LAYOUT["INST_WIFI_SIZE"], "inst_wifi"),
        )))
        with self.assertRaises(ValueError):
            self.build()

    def test_rejects_bad_installer(self) -> None:
        self.installer.write_bytes(esp_image(LAYOUT["INST_APP_SIZE"] + 16))
        with self.assertRaisesRegex(ValueError, "exceeds"):
            self.build()
        self.installer.write_bytes(esp_image(0x1000, chip=0))
        with self.assertRaisesRegex(ValueError, "ESP32-C3"):
            self.build()
        self.installer.write_bytes(b"\0" * 0x1000)
        with self.assertRaisesRegex(ValueError, "header"):
            self.build()

    def test_layout_constants_match_partitions_csv(self) -> None:
        rows = {}
        for line in (ROOT / "partitions.csv").read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                name, _, _, offset, size = [c.strip() for c in line.split(",")[:5]]
                rows[name] = (int(offset, 0), int(size, 0))
        self.assertEqual(rows["inst_state"], (LAYOUT["INST_STATE_OFFSET"], LAYOUT["INST_STATE_SIZE"]))
        self.assertEqual(rows["installer"], (LAYOUT["INST_APP_OFFSET"], LAYOUT["INST_APP_SIZE"]))
        self.assertEqual(rows["inst_wifi"], (LAYOUT["INST_WIFI_OFFSET"], LAYOUT["INST_WIFI_SIZE"]))
        self.assertEqual(sum(rows["inst_wifi"]), LAYOUT["INST_FLASH_SIZE"])
        self.assertEqual(sum(rows["factory"]), LAYOUT["INST_PLAY_LIMIT"])


if __name__ == "__main__":
    unittest.main()
