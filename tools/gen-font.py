#!/usr/bin/env python3
"""Bake Inter into the firmware's anti-aliased text atlases.

M5GFX's own fonts are 1-bit bitmaps: every glyph is a mask of on-or-off
pixels, so a curve is a staircase and a diagonal is a jag. That is why text on
the board looks rough no matter which face is chosen — it is the format, not
the typeface.

So the board draws its own: Inter is rasterised here at the sizes the screen
uses, each pixel kept as 4 bits of coverage, and the firmware blends a glyph
against what is under it. It is the same trick the magpie's bird uses, for the
same reason.

Inter is under the SIL Open Font License, so the atlases ship with this
project. The licence is copied beside them.

    python3 tools/gen-font.py            write firmware/include/ui_font.h
    python3 tools/gen-font.py --check    exit 1 if the file is out of date
"""

import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "firmware" / "include" / "ui_font.h"
LICENCE = ROOT / "firmware" / "include" / "Inter-LICENSE.txt"

# The faces the screen draws with, by role. A size is the em in pixels; the
# board's 320x240 needs few, and each costs flash.
#
#   11  the smallest label that is still readable at arm's length
#   14  body text: a model id, a quota's name
#   18  a section's value, a node's name
#   26  a hero number: how full the window is, today's calls
#   34  the one thing a page is about
FACES = [
    ("ui_11", "Inter-Medium.ttf", 11),
    ("ui_14", "Inter-Medium.ttf", 14),
    ("ui_18", "Inter-SemiBold.ttf", 18),
    ("ui_26", "Inter-SemiBold.ttf", 26),
    ("ui_34", "Inter-SemiBold.ttf", 34),
]

# ASCII printable, which is what a provider, a model and a vendor's window name
# are spelled with. Anything else draws as the fallback box.
FIRST, LAST = 0x20, 0x7E


def inter_dir() -> pathlib.Path:
    """Unpack Inter where the script can read it, once."""
    d = pathlib.Path(tempfile.gettempdir()) / "magpie-inter"
    if (d / "extras" / "ttf" / "Inter-Medium.ttf").exists():
        return d
    zipf = pathlib.Path(tempfile.gettempdir()) / "inter.zip"
    if not zipf.exists():
        url = "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip"
        subprocess.run(["curl", "-sL", "--max-time", "180", "-o", str(zipf), url], check=True)
    d.mkdir(parents=True, exist_ok=True)
    subprocess.run(["unzip", "-o", "-q", str(zipf), "-d", str(d)], check=True)
    return d


def render(ttf: pathlib.Path, px: int):
    """One face: its glyphs as coverage maps, and its line metrics."""
    from PIL import Image, ImageDraw, ImageFont

    font = ImageFont.truetype(str(ttf), px)
    ascent, descent = font.getmetrics()
    glyphs = []
    for cp in range(FIRST, LAST + 1):
        ch = chr(cp)
        adv = int(round(font.getlength(ch)))
        box = font.getbbox(ch)
        # bbox is (left, top, right, bottom) with top measured from the
        # ascender line, so a glyph's ink sits at -top above the baseline
        w, h = max(0, box[2] - box[0]), max(0, box[3] - box[1])
        alpha = []
        if w and h:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-box[0], -box[1]), ch, font=font, fill=255)
            # 8 bits of coverage down to the 4 the board keeps: 0..15
            alpha = [min(15, (v * 16 + 127) // 255) for v in img.getdata()]
        glyphs.append({
            "cp": cp,
            "w": w,
            "h": h,
            "xoff": box[0],
            # y offset from the baseline: negative is above it, as a font says
            "yoff": box[1] - ascent,
            "adv": adv,
            "a": alpha,
        })
    return {"ascent": ascent, "descent": descent, "height": ascent + descent, "glyphs": glyphs}


def emit(faces) -> str:
    head = '''// Baked by tools/gen-font.py. Do not edit.
//
// Inter, 4 bits of coverage a pixel. M5GFX's own fonts are 1-bit — a curve is
// a staircase — so the board draws its text from these instead, blending a
// glyph against what is under it. Inter is under the SIL Open Font License;
// the licence ships beside this file as Inter-LICENSE.txt.
//
// A glyph's `yoff` is measured from the baseline, upward as a font says it
// (negative above). `adv` is its advance in pixels.
#pragma once

#include <stdint.h>

namespace ui {
'''
    body = []
    for name, glyphs, ascent, height in faces:
        body.append(f"\n// {name}: ascent {ascent}, line height {height}\n")
        body.append(f"constexpr Glyph {name}_glyphs[] = {{\n")
        off = 0
        for g in glyphs:
            nbytes = (len(g["a"]) + 1) // 2
            body.append(
                f"  {{{g['cp']}, {g['w']}, {g['h']}, {g['xoff']}, {g['yoff']}, {g['adv']}, {off}}},\n")
            off += nbytes
        body.append("};\n")
        # the alpha, two pixels a byte, high nibble first
        body.append(f"constexpr uint8_t {name}_alpha[{max(1, off)}] = {{\n")
        line = []
        for g in glyphs:
            a = g["a"]
            for i in range(0, len(a), 2):
                hi = a[i]
                lo = a[i + 1] if i + 1 < len(a) else 0
                line.append(f"0x{(hi << 4) | lo:02x},")
            if not a:
                continue
            if len(line) >= 16:
                body.append("  " + "".join(line) + "\n")
                line = []
        if line:
            body.append("  " + "".join(line) + "\n")
        if not off:
            body.append("  0,\n")
        body.append("};\n")
        body.append(f"constexpr Face {name}{{{name}_glyphs, {len(glyphs)}, {name}_alpha, "
                    f"{ascent}, {height}}};\n")

    # the fallback: a hollow box, drawn where a codepoint has no glyph
    head += '''// Glyph is one character's ink: a coverage map of w x h at offset from the
// pen and the baseline, and how far the pen moves.
struct Glyph {
  uint16_t cp;
  uint8_t w, h;
  int8_t xoff, yoff;
  uint8_t adv;
  uint32_t off;  // into the face's alpha, two pixels a byte, high nibble first
};

// Face is one size of the typeface.
struct Face {
  const Glyph* glyphs;
  int count;
  const uint8_t* alpha;
  int ascent, height;
};
'''
    return head + "".join(body) + "\n}  // namespace ui\n"


def main() -> int:
    d = inter_dir()
    faces = []
    for name, ttf, px in FACES:
        f = render(d / "extras" / "ttf" / ttf, px)
        faces.append((name, f["glyphs"], f["ascent"], f["height"]))
        print(f"  {name}: {ttf} at {px}px, {f['height']}px line", file=sys.stderr)
    text = emit(faces)

    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text() != text:
            print("ui_font.h is out of date: run tools/gen-font.py", file=sys.stderr)
            return 1
        print("ui_font.h is current")
        return 0

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text)
    lic = d / "LICENSE.txt"
    if lic.exists():
        LICENCE.write_text(lic.read_text())
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
