#!/usr/bin/env python3
"""Check that every example's Passport Link INFO fits LINK_INFO_MAX.

passport_link_start() fails, and the device never advertises Link, when the
INFO JSON it builds from the settings schema is longer than LINK_INFO_MAX
bytes. This rebuilds the same JSON from the sources (PASSPORT_LINK_* settings,
channels, app_id, display_name, PROJECT_VER) so the gate catches it on the host.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LINK_CORE = ROOT / "components/passport_link/include/link_core.h"


def define(text, name):
    m = re.search(rf"#define\s+{name}\s+(\d+)u?\b", text)
    return int(m.group(1)) if m else None


def split_args(s):
    """Top-level comma split of a macro argument list; string literals kept."""
    args, cur, quote = [], "", False
    for i, c in enumerate(s):
        if c == '"' and (i == 0 or s[i - 1] != "\\"):
            quote = not quote
        if c == "," and not quote:
            args.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        args.append(cur.strip())
    return args


def value(arg, source):
    arg = arg.strip()
    if arg.startswith('"'):
        return json.loads(arg)
    if re.fullmatch(r"-?\d+", arg):
        return int(arg)
    resolved = define(source, re.escape(arg))
    if resolved is None:
        raise ValueError(f"cannot resolve {arg}")
    return resolved


def info_for(source, version):
    settings = []
    for kind, body in re.findall(r"PASSPORT_LINK_(BOOL_SETTING|INT|TEXT|ENUM)\((.*?)\),\s*\n", source):
        a = [value(x, source) for x in split_args(body)]
        item = {"k": a[0], "n": a[1]}
        if kind == "BOOL_SETTING":
            item.update(type="bool", **{"def": bool(a[2])})
        elif kind == "INT":
            item.update(type="int", **{"def": a[2], "min": a[3], "max": a[4]})
        elif kind == "TEXT":
            item.update(type="text", **{"def": a[2], "max": a[3]})
        else:
            item.update(type="enum", **{"def": a[2], "opt": a[3:]})
        settings.append(item)
    if not settings:
        return None
    channels = re.search(r"CHANNELS\[\]\s*=\s*\{(.*?)\}", source)
    info = {
        "v": 1,
        "id": re.search(r'\.app_id\s*=\s*"([^"]*)"', source).group(1),
        "name": re.search(r'\.display_name\s*=\s*"([^"]*)"', source).group(1),
        "ver": version,
        "ch": [value(c, source) for c in split_args(channels.group(1))] if channels else [],
        "set": settings,
    }
    return json.dumps(info, ensure_ascii=False, separators=(",", ":")).encode()


def main():
    limit = define(LINK_CORE.read_text(), "LINK_INFO_MAX")
    failed = False
    for app in sorted((ROOT / "examples").iterdir()):
        cmake = app / "CMakeLists.txt"
        if not cmake.exists():
            continue
        ver = re.search(r'PROJECT_VER\s+"([^"]*)"', cmake.read_text())
        # Without PROJECT_VER the version comes from git; allow a long one.
        version = ver.group(1) if ver else "v0.0.0-000-g0000000-dirty"
        for src in sorted((app / "main").glob("*.c")):
            text = src.read_text()
            if "passport_link_setting_t" not in text:
                continue
            info = info_for(text, version)
            if info is None:
                continue
            ok = len(info) <= limit
            failed |= not ok
            print(f"Link INFO {app.name}: {len(info)} / {limit} bytes {'PASS' if ok else 'FAIL'}")
    if failed:
        print("Shorten setting labels or merge settings; see docs/passport-link.md.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
