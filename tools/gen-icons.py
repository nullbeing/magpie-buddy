#!/usr/bin/env python3
"""Bake magpie's provider marks into the firmware's icon atlas.

The window draws a provider's mark beside its name; the board should too. But
the marks are SVG, and the board has no SVG parser — and half of magpie's icon
set is a multi-colour brand logo, which at 16 pixels on a 320px screen is a
smudge. So each mark is rasterised here as a single-colour silhouette: every
fill is forced to white, the ink is supersampled 4x and averaged down to 4 bits
of coverage, and the firmware tints it like it tints the bird.

The icons are magpie's own files (internal/gui/assets/icons), read only. They
are brand marks of their vendors and ship under whatever terms those vendors
set; this project only rasterises the ones a board can show.

    python3 tools/gen-icons.py            write firmware/include/icons.h
    python3 tools/gen-icons.py --check    exit 1 if the file is out of date
"""

import base64
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
MAGPIE = ROOT.parent
ICONS = MAGPIE / "internal" / "gui" / "assets" / "icons"
OUT = ROOT / "firmware" / "include" / "icons.h"
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

# One 16x16 tile a mark. A provider's own icon name in magpie is the file stem:
# the -color variants are what the window uses, and they silhouette just as
# well as the plain ones.
SIZE = 20
MARKS = [
    ("codex", "codex-color"),
    ("claude", "claudecode-color"),
    ("anthropic", "claude-color"),
    ("deepseek", "deepseek-color"),
    ("openai", "openai"),
    ("google", "gemini-color"),
    ("copilot", "githubcopilot"),
    ("kimi", "kimi"),
    ("grok", "grok"),
    ("opencode", "opencode"),
]


def page() -> str:
    """One page that rasterises every mark and reports its coverage."""
    jobs = []
    for name, stem in MARKS:
        path = ICONS / (stem + ".svg")
        if not path.exists():
            sys.exit(f"gen-icons: no {path}")
        svg = path.read_text()
        m = re.search(r'viewBox="([^"]+)"', svg)
        if not m:
            sys.exit(f"gen-icons: {stem}.svg has no viewBox")
        jobs.append({"name": name, "box": m.group(1), "svg": svg})

    return f"""<!DOCTYPE html><html><body><script>
const jobs = {json.dumps(jobs)};
const SIZE = {SIZE};

// silk turns a mark into a silhouette: every shape one colour, so what is left
// is its outline and not its brand palette. A shape that says fill="none" is an
// outline by design and keeps its stroke instead.
function silk(svg) {{
  const css = `<style>
    path:not([fill="none"]),circle:not([fill="none"]),rect:not([fill="none"]),
    ellipse:not([fill="none"]),polygon:not([fill="none"]),polyline:not([fill="none"]) {{ fill:#fff !important; }}
    path[stroke]:not([stroke="none"]),line,circle[stroke],rect[stroke],ellipse[stroke],
    polyline[stroke],polygon[stroke] {{ stroke:#fff !important; }}
  </style>`;
  return svg.replace(/<svg([^>]*)>/, "<svg$1>" + css);
}}

async function draw(job) {{
  const SS = 4, W = SIZE * SS;
  let svg = silk(job.svg);
  // the source is 1em square; give it the pixels we want and keep the viewBox
  svg = svg.replace(/width="[^"]*"/, `width="${{W}}"`).replace(/height="[^"]*"/, `height="${{W}}"`);
  const img = new Image();
  img.src = "data:image/svg+xml;base64," + btoa(unescape(encodeURIComponent(svg)));
  await img.decode();
  const c = document.createElement("canvas");
  c.width = W; c.height = W;
  const g = c.getContext("2d");
  g.clearRect(0, 0, W, W);
  g.drawImage(img, 0, 0, W, W);
  const d = g.getImageData(0, 0, W, W).data;
  const out = new Uint8Array(SIZE * SIZE);
  for (let y = 0; y < SIZE; y++) {{
    for (let x = 0; x < SIZE; x++) {{
      let sum = 0;
      for (let sy = 0; sy < SS; sy++) {{
        for (let sx = 0; sx < SS; sx++) {{
          const i = (((y * SS + sy) * W) + (x * SS + sx)) * 4 + 3;
          sum += d[i];
        }}
      }}
      out[y * SIZE + x] = Math.min(15, Math.round(sum / (SS * SS) / 17));
    }}
  }}
  return {{ name: job.name, w: SIZE, h: SIZE, a: Array.from(out) }};
}}

(async () => {{
  const out = [];
  for (const job of jobs) out.push(await draw(job));
  document.title = "done";
  const p = document.createElement("pre");
  p.textContent = "MARKS" + JSON.stringify(out) + "END";
  document.body.appendChild(p);
}})();
</script></body></html>"""


def render() -> list:
    if not pathlib.Path(CHROME).exists():
        sys.exit(f"gen-icons: {CHROME} isn't there")
    with tempfile.NamedTemporaryFile("w", suffix=".html", delete=False) as fh:
        fh.write(page())
        path = fh.name
    out = subprocess.run(
        [CHROME, "--headless", "--disable-gpu", "--dump-dom",
         "--virtual-time-budget=8000", f"file://{path}"],
        capture_output=True, text=True, timeout=180,
    )
    m = re.search(r"MARKS(\[.*?\])END", out.stdout, re.S)
    if not m:
        sys.exit("gen-icons: the page didn't report the masks\n" + out.stdout[-2000:] + out.stderr[-2000:])
    return json.loads(m.group(1))


def emit(marks: list) -> str:
    head = '''// Baked by tools/gen-icons.py. Do not edit.
//
// magpie's provider marks as one-colour silhouettes, 4 bits of coverage a
// pixel at 16x16. The window shows them in a brand's own colour; the board
// cannot hold a palette a mark, and at this size a multi-colour logo is a
// smudge, so the ink is what carries the identity and the page tints it.
//
// A mark is looked up by the provider id the frame carries, with the prefix
// match that covers a plugin's ids ("opencode-zen-free" is opencode's mark); a
// provider with no mark here draws no icon rather than a placeholder.
#pragma once

#include <stdint.h>
#include <string.h>

namespace magpie {

// Icon is a mark's ink: a coverage map of w x h at 16 levels.
struct Icon {
  const uint8_t* alpha;
  int w, h;
};

'''
    body = []
    for m in marks:
        name, w, h, a = m["name"], m["w"], m["h"], m["a"]
        body.append(f"\n// {name}: {w}x{h}, one nibble a pixel, 0 transparent, 15 opaque.\n")
        body.append(f"constexpr uint8_t icon_{name}_alpha[{w * h // 2 + 1}] = {{\n")
        line = []
        for i in range(0, len(a), 2):
            hi = a[i]
            lo = a[i + 1] if i + 1 < len(a) else 0
            line.append(f"0x{(hi << 4) | lo:02x},")
            if (i // 2) % 16 == 15:
                body.append("  " + "".join(line) + "\n")
                line = []
        if line:
            body.append("  " + "".join(line) + "\n")
        body.append("};\n")
        body.append(f"constexpr Icon icon_{name}{{icon_{name}_alpha, {w}, {h}}};\n")

    body.append('''
// iconFor is the provider id a frame carries to the mark it is known by. The
// longest prefix wins where two are given, so a specific id is not swallowed by
// a shorter one.
inline const Icon* iconFor(const char* id) {
  if (!id || !*id) return nullptr;
  struct Row {
    const char* prefix;
    const Icon* ic;
  };
  static const Row kRows[] = {
      {"codex", &icon_codex},
      {"claude", &icon_claude},
      {"anthropic", &icon_anthropic},
      {"deepseek", &icon_deepseek},
      {"openai", &icon_openai},
      {"chatgpt", &icon_openai},
      {"google", &icon_google},
      {"gemini", &icon_google},
      {"copilot", &icon_copilot},
      {"kimi", &icon_kimi},
      {"moonshot", &icon_kimi},
      {"grok", &icon_grok},
      {"xai", &icon_grok},
      {"opencode", &icon_opencode},
  };
  const Icon* best = nullptr;
  size_t bestLen = 0;
  for (const Row& r : kRows) {
    const size_t n = strlen(r.prefix);
    if (n > bestLen && strncmp(id, r.prefix, n) == 0) {
      best = r.ic;
      bestLen = n;
    }
  }
  return best;
}

}  // namespace magpie
''')
    return head + "".join(body)


def main() -> int:
    marks = render()
    text = emit(marks)
    if "--check" in sys.argv:
        if OUT.exists() and OUT.read_text() == text:
            print("icons.h is current", file=sys.stderr)
            return 0
        print("icons.h is out of date", file=sys.stderr)
        return 1
    OUT.write_text(text)
    print(f"wrote {OUT.relative_to(ROOT)} ({len(marks)} marks)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
