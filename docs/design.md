# How this is built

The parts of the design that the README does not need to say, but that someone
changing the code will want.

## The preview and the renderer

Two things draw this screen, and they answer different questions:

| | `preview/flight.html` | `tools/render/` |
| --- | --- | --- |
| What it draws | an independent drawing of the design | **the firmware's own `screen.cpp`**, compiled |
| Answers | "what should it look like" | "what will the board actually draw" |
| Depends on | the browser | a C++ compiler |

The preview is the design. The renderer is the check: it compiles the file the
board draws with, against a shim that turns M5GFX calls into SVG, so a layout
change can be seen — and diffed — without a board on USB.

```sh
make -C tools/render                  # every state, into tools/render/out/
make -C tools/render one STATE=route  # just one
```

The states are `offline`, `route`, `context`, `usage`, `limits`, `failover`,
`tight`, `flying`, `failed`, and the board's own `setup` and `setup-failed`.
Each is written twice, dark and light.

The preview's own URL parameters:

| URL | What it does |
| --- | --- |
| (none) | The controls: play the demo, send one success, one failover, or paste a real frame |
| `?auto=1` | Plays the demo on load |
| `?frame=1` | Draws one sample frame, with the bird mid-flight, and stops — the same picture every time, for a screenshot or a diff |

Paste a real frame into the box to see exactly what the board would draw:

```sh
node bridge/magpie-display.mjs --dry-run --once   # the frame it would push
curl -s http://magpie-desk.local:9877/health      # what the board last drew
```

## The text is the board's own

M5GFX's fonts are 1-bit — a glyph is a mask of on-or-off pixels, so every curve
is a staircase, and that is why text on a board like this looks rough whatever
face it is given. So the firmware draws its own text from `ui_font.h`: Inter
rasterised at the sizes the screen uses, 4 bits of coverage a pixel, blended
against what is under it.

`tools/gen-font.py` bakes it, `ui_text.h` draws it, and both the firmware and
the renderer compile the same two files — so the preview shows the board's
letterforms, not a browser's approximation of them.

```sh
python3 tools/gen-font.py            # rebake the atlases
python3 tools/gen-font.py --check    # exit 1 if they are out of date
```

Inter is under the SIL Open Font License; the licence is copied beside the
header as `Inter-LICENSE.txt`.

The same trick, same file format, for magpie's bird (`tools/gen-bird.py`) and
the provider marks (`tools/gen-icons.py`). A mask is one nibble a pixel, 0
transparent, 15 opaque, and it is blended at runtime in whatever colour the page
wants — which is how the bird takes its agent's colour.

## How the flight works

It is magpie's Routing page, step for step, at its own timings
(`internal/gui/assets/plugins/routing/client.js`, `play()`):

| Step | Time |
| --- | --- |
| The request leaves the agent, through magpie, to the account | 1400 ms (900 ms when magpie is already carrying it) |
| The bird lets go; the dot waits at the account while it answers | 900 ms |
| Answered: the dot comes home the way it went | 1400 ms |
| Failed, and the routing will try again: back to magpie | 620 ms |
| Failed, and that was the answer: home, red | 1350 ms |

The bird is magpie's own shape, and its wings beat with the window's own
`@keyframes flap`: `tools/gen-bird.py` renders four frames of it with headless
Chrome and bakes them as 4-bit alpha masks, so the board is not a second
interpretation of the drawing that could drift from it.

## What a frame does not say

A display sits in a room other people walk through, so a frame names providers,
models and agents, and nothing else:

- no account, email, key name or key id;
- no session, request id or folder;
- no prompt text, reply text or error text.

The trace the bridge reads carries more than a frame does — an account in
`tries[].id`, the account that answered in `affinity`, one on each quota card,
the session and folder — and the bridge drops all of it. [The bridge's
README](../bridge/README.md#privacy) has the table of what is dropped.

The token is not sent by discovery, either: a probe is broadcast, so an answer
that named the token would hand it to whoever asked. It is set on both ends by
hand.

## Verifying

```sh
# the bridge, against a running magpie: one frame, printed
node bridge/magpie-display.mjs --dry-run --once

# every baked atlas is up to date
python3 tools/gen-bird.py --check
python3 tools/gen-icons.py --check
python3 tools/gen-setup-page.py --check
python3 tools/gen-font.py --check

# the firmware builds for the board
cd firmware && pio run

# the renderer builds, which is what proves screen.cpp stayed free of the board
make -C tools/render
```

The preview's JavaScript is checked the way the firmware's output is:

```sh
python3 - <<'PY'
import re; src = open("preview/flight.html").read()
open("/tmp/flight.js","w").write(re.findall(r"<script>(.*?)</script>", src, re.S)[-1])
PY
node --check /tmp/flight.js
```
