#!/usr/bin/env python3
"""Generate the Chinese LVGL fonts of the Wi-Fi weather example.

Source font: Noto Sans SC Regular, notofonts/noto-cjk tag Sans2.004,
Sans/SubsetOTF/SC/NotoSansSC-Regular.otf
(SHA-256 faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9),
SIL Open Font License 1.1 (fonts/OFL.txt). The font file is not committed;
download it and pass its path.

Converter: lv_font_conv 1.5.3 (npm). Output: main/fonts/wx_font_zh_{14,20}.c,
4 bpp, uncompressed, for LVGL 9.5.

Coverage:
  14 px  printable ASCII, a few symbols, and every non-ASCII character of
         main/weather_i18n.c (fixed screen text).
  20 px  the same plus the 3755 GB2312 level-1 characters, so Chinese place
         names pushed by the phone render. Rarer characters show the LVGL
         placeholder.

Usage: generate_fonts.py /path/to/NotoSansSC-Regular.otf /path/to/lv_font_conv
"""
import hashlib
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent / "main"
FONT_SHA256 = "faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9"
EXTRA = "°•、。（），：！？"   # ° • 、。（），：！？


def ui_characters() -> str:
    text = (MAIN / "weather_i18n.c").read_text(encoding="utf-8")
    return "".join(sorted({c for c in text if ord(c) > 0x7E}))


def gb2312_level1() -> str:
    chars = []
    for row in range(0xB0, 0xD8):
        for col in range(0xA1, 0xFF):
            if row == 0xD7 and col > 0xF9:
                break
            chars.append(bytes([row, col]).decode("gb2312"))
    assert len(chars) == 3755, len(chars)
    return "".join(chars)


def convert(converter: str, font: Path, size: int, symbols: str) -> None:
    name = f"wx_font_zh_{size}"
    subprocess.run([
        converter, "--font", str(font), "-r", "0x20-0x7E", "--symbols", symbols,
        "--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
        "--lv-font-name", name, "--lv-include", "lvgl.h",
        "-o", str(MAIN / "fonts" / f"{name}.c"),
    ], check=True)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    font, converter = Path(sys.argv[1]), sys.argv[2]
    digest = hashlib.sha256(font.read_bytes()).hexdigest()
    if digest != FONT_SHA256:
        print(f"unexpected font file (sha256 {digest})", file=sys.stderr)
        return 1
    ui = "".join(sorted(set(ui_characters() + EXTRA)))
    convert(converter, font, 14, ui)
    convert(converter, font, 20, "".join(sorted(set(ui + gb2312_level1()))))
    print(f"14 px: {len(ui)} non-ASCII characters; 20 px: {len(set(ui + gb2312_level1()))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
