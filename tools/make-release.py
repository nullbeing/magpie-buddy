#!/usr/bin/env python3
"""Build one flashable image, and the manifest a browser flasher reads.

The board is flashed over a cable with four separate images at four offsets.
A browser cannot do that: ESP Web Tools wants one file written from address 0,
so this merges them the way esptool does and drops the result where the flasher
page serves it.

    python3 tools/make-release.py            build docs/flasher/ from firmware/
    python3 tools/make-release.py --version 0.2.0
    python3 tools/make-release.py --check    exit 1 if the page's image is stale
"""

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "firmware" / ".pio" / "build" / "cores3"
OUT = ROOT / "docs" / "flasher"
IMAGE = "magpie-buddy.bin"
MANIFEST = "manifest.json"

# The four images a CoreS3 boots from, and where each goes. These offsets are
# the partition table's, not a choice: 0x0 bootloader, 0x8000 the table itself,
# 0xe000 the OTA selector, 0x10000 the application.
LAYOUT = [
    (0x0, "bootloader.bin"),
    (0x8000, "partitions.bin"),
    (0xE000, None),  # boot_app0.bin, which lives in the framework package
    (0x10000, "firmware.bin"),
]


def esptool() -> pathlib.Path:
    found = sorted(pathlib.Path.home().glob(".platformio/packages/tool-esptoolpy@*/esptool.py"))
    if not found:
        sys.exit("make-release: no esptool.py in ~/.platformio/packages")
    return found[-1]


def boot_app0() -> pathlib.Path:
    found = sorted(
        pathlib.Path.home().glob(
            ".platformio/packages/framework-arduinoespressif32@*/tools/partitions/boot_app0.bin"
        )
    )
    if not found:
        sys.exit("make-release: no boot_app0.bin in the framework package")
    return found[-1]


def version() -> str:
    """The version this repository is on: its git tag, or 0.0.0 before one."""
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        )
        v = out.stdout.strip()
    except subprocess.CalledProcessError:
        return "0.0.0"
    return v[1:] if v.startswith("v") else v


def merge(dest: pathlib.Path) -> None:
    for offset, name in LAYOUT:
        path = boot_app0() if name is None else BUILD / name
        if not path.exists():
            sys.exit(f"make-release: {path} is missing — run `pio run -d firmware` first")
    args = [sys.executable, str(esptool()), "--chip", "esp32s3", "merge_bin",
            "-o", str(dest), "--flash_mode", "dio", "--flash_freq", "80m",
            "--flash_size", "16MB"]
    for offset, name in LAYOUT:
        args += [hex(offset), str(boot_app0() if name is None else BUILD / name)]
    # esptool needs pyserial, which PlatformIO's own python has; the system one
    # may not. uv is how this repo runs its other generators.
    out = subprocess.run(
        ["uv", "run", "--with", "pyserial", "python3"] + args[1:],
        capture_output=True, text=True,
    )
    if out.returncode != 0:
        sys.exit("make-release: esptool failed\n" + out.stdout + out.stderr)


def manifest(v: str, size: int, sha: str) -> str:
    return json.dumps({
        "name": "Magpie Buddy",
        "version": v,
        "new_install_prompt_erase": True,
        # A CoreS3 is the only board this firmware is built for, and saying so
        # makes the page refuse a different chip instead of half-flashing it.
        "builds": [{
            "chipFamily": "ESP32-S3",
            "parts": [{"path": IMAGE, "offset": 0}],
        }],
    }, indent=2) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=None)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    v = args.version or version()
    OUT.mkdir(parents=True, exist_ok=True)
    target = OUT / IMAGE

    if args.check:
        if not target.exists():
            print("make-release: no image in docs/flasher", file=sys.stderr)
            return 1
        fresh = pathlib.Path(tempfile.gettempdir()) / "magpie-buddy-check.bin"
        merge(fresh)
        same = fresh.read_bytes() == target.read_bytes()
        print(f"image {'is current' if same else 'is STALE'}", file=sys.stderr)
        return 0 if same else 1

    merge(target)
    data = target.read_bytes()
    (OUT / MANIFEST).write_text(manifest(v, len(data), hashlib.sha256(data).hexdigest()))
    print(f"wrote docs/flasher/{IMAGE} ({len(data)} bytes) and {MANIFEST} at {v}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
