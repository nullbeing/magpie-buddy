#!/usr/bin/env python3
"""Rasterise magpie's own bird into the firmware's bitmaps.

The flight and the hub glyph are the same shapes magpie's window draws
(internal/gui/assets/plugins/routing/client.js FLIER, and index.html's #bird),
so the desk display's magpie is the same magpie. The paths are big and the
board is small, so they are baked here rather than parsed on the ESP32: one
4-bit alpha mask per size and per wingbeat frame, blended against the stage's
background at runtime, which is what lets the bird take the agent's colour.

The wingbeat is the page's own: the same `@keyframes flap` at the same .42s,
sampled at four points and paused. So the board's bird beats its wings the way
the window's does, rather than a second animation that drifts from it.

Rendering is done by headless Chrome — the engine the preview is checked in —
so the mask is what the browser draws, not a second reading of the SVG.

    python3 tools/gen-bird.py            write firmware/include/bird.h
    python3 tools/gen-bird.py --check    exit 1 if the file is out of date
"""

import base64
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
# The magpie checkout this project sits in: its window's bird paths are the
# shapes baked here. Read only; nothing in this script writes to magpie.
MAGPIE = ROOT.parent
OUT = ROOT / "firmware" / "include" / "bird.h"
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

# The flight's magpie, as magpie's Routing page draws it: facing right, the dot
# it carries at the tip of its beak, its wings beating from the shoulder. The
# markup is the page's own (client.js FLIER).
FLIER = (
    '<g transform="scale(1.2) translate(-6.5 3.2)">'
    '<path class="wing far" d="M-14.5 -3.4C-16.5 -9.5 -14.2 -15 -8.6 -18.6C-9.4 -12.6 -9.6 -7.4 -9.2 -3.2Z"/>'
    '<path class="tail" d="M-31.5 3.1L-18.6 -1.8L-17.4 1.2L-30.8 4.6Z"/>'
    '<ellipse class="body" cx="-12.2" cy="-1.4" rx="7.6" ry="3.9"/>'
    '<ellipse class="belly" cx="-12.8" cy="0.4" rx="4.3" ry="1.6"/>'
    '<circle class="body" cx="-4.9" cy="-3.7" r="3.1"/>'
    '<path class="body" d="M-2.3 -4.8L1.4 -3.3L-2.3 -2.2Z"/>'
    '<g class="wing near"><path d="M-15.6 -3.2C-17.8 -10.2 -15.2 -16.4 -8.4 -20.4C-9.3 -13.6 -9.4 -7.8 -8.8 -2.8Z"/>'
    '<path class="bar" d="M-14.6 -5.6C-15.4 -10.4 -13.8 -14.4 -10.4 -17.2"/></g>'
    "</g>"
)

# The page's own rules for it (routing.css), with the two colours pinned: the
# silhouette is white, and what the card's colour shows through — the belly,
# the wing's bar — is black. The flap is the page's keyframes, word for word.
FLIER_CSS = """
.flier { color: #fff; }
.flier .body, .flier .tail, .flier .wing path:first-child, .flier .wing.far { fill: currentColor; }
.flier .wing.far { opacity: .45; }
.flier .belly { fill: #000; }
.flier .wing .bar { fill: none; stroke: #000; stroke-width: 1.3; stroke-linecap: round; }
.flier .wing { transform-box: fill-box; transform-origin: 50% 100%;
               animation: flap .42s cubic-bezier(.45, 0, .3, 1) infinite; }
.flier .wing.far { animation-delay: -.03s; }
@keyframes flap { 0%, 100% { transform: scaleY(1); } 55% { transform: scaleY(-.62) skewX(-8deg); } }
"""

# The wingbeat, sampled. Four is the fewest that reads as a beat at 12 fps:
# level, on the way up, over, on the way down. The delays are fractions of the
# .42s cycle, and the near wing's is the one the page offsets the far one from.
FLAP = [0.00, 0.14, 0.23, 0.32]
FLAP_S = 0.42

# name: (width, height, viewBox, frames)
#
# The flight's viewBox is the path's own bounds with a pixel of margin: the
# magpie spans x -31.5..1.4 and y -20.4..4.6 (facing right, the dot at its
# beak), so anything tighter cuts off the tail and the belly. The wing's raised
# frames need the room above y -20.4 too, hence the tall box.
JOBS = [
    ("bird_flight", 34, 30, "-33 -26 36 32", FLAP),
    # The card stage leaves 33px between nodes. The window's own gap is 56px
    # with a ~40px bird, so 24px here is the same proportion of the gap; the
    # 34px sprite in 33px was a bird wider than the space it had to fly in.
    ("bird_flight_s", 24, 21, "-33 -26 36 32", FLAP),
    ("bird_hub", 22, 22, "0 0 44 44", None),
]


# The jobs that are a wingbeat rather than one still image. render() returns
# only name/w/h/a/i, so this is what emit() keys the per-frame naming on.
FRAMED = {name for name, _w, _h, _box, frames in JOBS if frames is not None}


def page() -> str:
    """One page that renders every job into a canvas and reports its alpha."""
    hub = (MAGPIE / "internal" / "gui" / "assets" / "index.html").read_text()
    m = re.search(r'<path id="bird"[^>]*\bd="([^"]+)"', hub)
    if not m:
        sys.exit("gen-bird: index.html has no #bird path any more")
    hub_svg = '<path fill-rule="evenodd" fill="#fff" d="' + m.group(1) + '"/>'

    jobs = []
    for name, w, h, box, frames in JOBS:
        for i, t in enumerate(frames or [None]):
            jobs.append({"name": name, "w": w, "h": h, "box": box, "i": i, "t": t})

    return f"""<!DOCTYPE html><html><body>
<script>
const FLIER = {json.dumps(FLIER)};
const FLIER_CSS = {json.dumps(FLIER_CSS)};
const HUB = {json.dumps(hub_svg)};
const FLAP_S = {FLAP_S};
const jobs = {json.dumps(jobs)};

// draw renders one job at 4x and averages down to 4-bit alpha: the same
// supersampling the window's own antialiasing does, done once here. A frame
// with t set is the wing paused t into the page's cycle, which is what makes
// the board's beat the page's beat.
function draw(job) {{
  const SS = 4, W = job.w * SS, H = job.h * SS;
  const c = document.createElement("canvas");
  c.width = W; c.height = H;
  const g = c.getContext("2d");
  g.clearRect(0, 0, W, H);
  g.fillStyle = "#fff";
  let inner;
  if (job.name === "bird_hub") {{
    inner = HUB;
  }} else {{
    // animation-delay negative puts it t into the cycle; paused holds it
    // there, so the render is that instant and not whichever the engine
    // reached first
    const css = FLIER_CSS + `\\n.flier .wing {{ animation-delay: -${{(job.t * FLAP_S).toFixed(4)}}s;
                 animation-play-state: paused; }}`;
    inner = `<style>${{css}}</style><g class="flier">${{FLIER}}</g>`;
  }}
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${{job.box}}" width="${{W}}" height="${{H}}">${{inner}}</svg>`;
  const img = new Image();
  img.src = "data:image/svg+xml;base64," + btoa(unescape(encodeURIComponent(svg)));
  return img.decode().then(() => {{
    g.drawImage(img, 0, 0, W, H);
    const d = g.getImageData(0, 0, W, H).data;
    const out = new Uint8Array(job.w * job.h);
    for (let y = 0; y < job.h; y++) {{
      for (let x = 0; x < job.w; x++) {{
        let sum = 0;
        for (let sy = 0; sy < SS; sy++) for (let sx = 0; sx < SS; sx++) {{
          sum += d[(((y * SS + sy) * W) + (x * SS + sx)) * 4 + 3];
        }}
        out[y * job.w + x] = Math.round(sum / (SS * SS) / 17);  // 0..15
      }}
    }}
    return {{ name: job.name, i: job.i, w: job.w, h: job.h, a: Array.from(out) }};
  }});
}}

(async () => {{
  const res = [];
  for (const j of jobs) res.push(await draw(j));
  document.title = "done";
  document.body.textContent = "BIRDS" + JSON.stringify(res) + "END";
}})();
</script></body></html>"""


def render() -> list:
    if not pathlib.Path(CHROME).exists():
        sys.exit(f"gen-bird: {CHROME} isn't there")
    with tempfile.NamedTemporaryFile("w", suffix=".html", delete=False) as fh:
        fh.write(page())
        path = fh.name
    out = subprocess.run(
        [CHROME, "--headless", "--disable-gpu", "--dump-dom",
         "--virtual-time-budget=8000", f"file://{path}"],
        capture_output=True, text=True, timeout=180,
    )
    m = re.search(r"BIRDS(\[.*?\])END", out.stdout, re.S)
    if not m:
        sys.exit("gen-bird: the page didn't report the masks\n" + out.stdout[-2000:] + out.stderr[-2000:])
    return json.loads(m.group(1))


def emit(birds: list) -> str:
    head = '''// Baked by tools/gen-bird.py. Do not edit.
//
// magpie's own bird, 4-bit alpha: the flight's, in the four frames of the
// window's wingbeat, and the hub's mark. The paths are magpie's window's
// (internal/gui/assets/plugins/routing/client.js FLIER, index.html #bird), and
// the beat is its own @keyframes flap sampled at four points, so the desk
// display's magpie is the same magpie, beating its wings the same way. At
// runtime a mask is blended against the stage's background in the colour of
// the agent it flies for, which is why it is alpha and not pixels.
#pragma once

#include <stdint.h>

namespace magpie {
'''
    body = []
    for b in birds:
        name = b["name"]
        w, h, a = b["w"], b["h"], b["a"]
        if b["name"] in FRAMED:
            # a framed job emits one array a frame, named by index — whichever
            # job it is. Keying this on the name "bird_flight" gave every frame
            # of a second framed job the same symbol, so they overwrote.
            stem = f"{name}{b['i']}"
            body.append(f"\n// {stem}: {w}x{h}, the wing at frame {b['i']} of the beat.\n")
        else:
            stem = name
            body.append(f"\n// {name}: {w}x{h}, one nibble a pixel, 0 transparent, 15 opaque.\n")
        body.append(f"constexpr int {stem}_w = {w};\n")
        body.append(f"constexpr int {stem}_h = {h};\n")
        body.append(f"constexpr uint8_t {stem}_alpha[{w * h // 2 + 1}] = {{\n")
        # two pixels a byte, high nibble first
        for i in range(0, len(a), 2):
            hi = a[i]
            lo = a[i + 1] if i + 1 < len(a) else 0
            body.append(f"0x{(hi << 4) | lo:02x},")
            if (i // 2) % 16 == 15:
                body.append("\n")
        body.append("\n};\n")
    # one table a framed job, so the firmware walks its frames by index and a
    # frame count change stays a data change
    framed = []
    for b in birds:
        if b["name"] in FRAMED and b["name"] not in framed:
            framed.append(b["name"])
    if framed:
        body.append("\n// A wingbeat, in order. One entry a frame, at 12 fps.\n")
        body.append("struct BirdFrame {\n  const uint8_t* alpha;\n  int w, h;\n};\n")
    for name in framed:
        frames = [b for b in birds if b["name"] == name]
        body.append(f"constexpr BirdFrame {name}[{len(frames)}] = {{\n")
        for b in frames:
            stem = f"{name}{b['i']}"
            body.append(f"    {{{stem}_alpha, {stem}_w, {stem}_h}},\n")
        body.append("};\n")
        body.append(f"constexpr int {name}_frames = {len(frames)};\n")
    return head + "".join(body) + "\n}  // namespace magpie\n"


def main() -> int:
    birds = render()
    text = emit(birds)
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text() != text:
            print("bird.h is out of date: run tools/gen-bird.py", file=sys.stderr)
            return 1
        print("bird.h is current")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text)
    total = sum(len(b["a"]) for b in birds)
    print(f"wrote {OUT.relative_to(ROOT)} ({total} pixels, {len(birds)} masks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
