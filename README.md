# Magpie Buddy

A small screen on the desk that shows what magpie is doing right now: which
agent asked, which account answered, how full the context window is, and what
is left on each allowance. When a request goes out, a magpie flies from the
agent to magpie and on to the account — and when an account fails, it turns
back and takes the request on to the next.

Hardware: **M5Stack CoreS3** (ESP32-S3, 320×240, 8 MB PSRAM, Wi-Fi).

```
   ┌──────────────────────────────────────────┐
   │ claude                kimi/kimi-k2.5     │   header: who asked, what answered
   │                       group opus-anywhere│
   │                                          │
   │  ┌────┐      ┌────┐        ┌──────┐      │   the stage: agent → magpie → account
   │  │ ●  │──────│ 🐦 │───────▶│  ●   │      │
   │  │claude      magpie       kimi         │
   │  └────┘      └────┘        └──────┘      │
   │                                          │
   │   ◜◝     today        Claude 5h   62%    │   context arc, today, allowances
   │   24%    37 calls     Codex 7d    18%    │
   │   48k    1.2M tok     Kimi 月     84%    │
   └──────────────────────────────────────────┘
```

## What is here

| Path | What it is |
| --- | --- |
| `docs/protocol.md` | The wire protocol: discovery, frames, timing, and what a frame deliberately does not say |
| `preview/flight.html` | The screen and the flight, at 320×240, in a browser. **The design reference**: the firmware draws what this draws |
| `firmware/` | The board's firmware (PlatformIO, Arduino, M5Unified) |
| `firmware/include/bird.h` | The magpie's masks, baked from magpie's own SVG by `tools/gen-bird.py` |
| `tools/gen-bird.py` | Rasterises magpie's bird — including the window's own wingbeat — into that header |
| `firmware/include/ui_font.h` | **Inter, 4 bits of coverage a pixel**, baked by `tools/gen-font.py` |
| `tools/gen-font.py` | Rasterises Inter into that header; the licence ships beside it |
| `tools/render/` | **Draws the board's screen without the board.** Compiles `firmware/src/screen.cpp` against a shim that emits SVG, so a layout can be seen before it is flashed |
| `bridge/` | **The magpie side.** One Node file that serves the board from any magpie; see its own [README](bridge/README.md) |

## Getting it running

### 1. The bridge (magpie side)

```sh
node bridge/magpie-display.mjs --discover
```

That is the whole setup. No install, no build, no dependencies: it uses three
Node built-in modules and reads what magpie already serves on its own local API.

**It is a bridge and not a plugin on purpose.** magpie's plugins cannot do this:

| Kind | Installed with | Why not |
| --- | --- | --- |
| Built-in plugin (`plugins/display` in magpie's source) | nothing — it is compiled in | Adding one means editing `plugins/all/all.go` and rebuilding magpie. That is not something a board's package can ship, and it would fork magpie. |
| JS gateway plugin | `magpie plugin add ./x.mjs` | Runs on moejs *inside* the gateway, which has no network access: no `fetch`, no sockets. It can rewrite requests; it cannot reach a board. |

So this package touches no magpie source. It only reads three GETs the gateway
already answers, and pushes the v1 frames the firmware expects. **The firmware
does not know which side is pushing it.**

Set a token if you want one, and give the board the same string.

### 2. The firmware (board side)

```sh
cd firmware
pio run -t upload        # over USB-C
pio device monitor       # 115200, the serial log
```

Then set it up from a phone — no rebuild, no password in the source:

1. The board raises an access point called **`Magpie-Buddy-XXXX`** (the last two
   bytes of its chip id) and shows the name on its own screen.
2. Join it. The setup page opens by itself; if it doesn't, go to
   **`http://192.168.4.1`**.
3. Pick your network, type the password, connect. The board saves it to NVS and
   restarts onto your network.

The network lives in NVS, so a power cut does not lose it, and moving the board
to another house only means running setup again — **hold the screen for three
seconds** to forget the saved network and bring the access point back.

`config.h` is no longer needed. A board built with one still joins the network
compiled into it, so an older build keeps working; leave the file out and the
board provisions over the air instead. If you would rather not use a phone,
`join <ssid> <password>` over the serial port does the same thing, and `forget`
clears it.

### 3. Watch it

```sh
```

The board broadcasts `MAGPIE-DISPLAY?` every 30 s until it has been pushed to,
so its address never has to be typed into anything. The bridge answers it (with
`--discover`). Its own `/health` answers what it last drew:

```sh
curl -s http://magpie-desk.local:9877/health
```

## The preview

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

The states are `offline`, `route`, `context`, `usage`, `failover`, `tight`,
`flying` and `failed`.

**The text is the board's own.** M5GFX's fonts are 1-bit — a glyph is a mask of
on-or-off pixels, so every curve is a staircase, and that is why text on a board
like this looks rough whatever face it is given. So the firmware draws its own
text from `ui_font.h`: Inter rasterised at the sizes the screen uses, 4 bits of
coverage a pixel, blended against what is under it. `tools/gen-font.py` bakes
it, `ui_text.h` draws it, and both the firmware and this renderer compile the
same two files — so the preview shows the board's letterforms, not a browser's
approximation of them.

```sh
python3 tools/gen-font.py            # rebake the atlases
python3 tools/gen-font.py --check    # exit 1 if they are out of date
```

Inter is under the SIL Open Font License; the licence is copied beside the
header as `Inter-LICENSE.txt`.

| URL | What it does |
| --- | --- |
| (none) | The controls: play the demo, send one success, one failover, or paste a real frame |
| `?auto=1` | Plays the demo on load |
| `?frame=1` | Draws one sample frame, with the bird mid-flight, and stops — the same picture every time, for a screenshot or a diff |

Paste a real frame into the box to see exactly what the board would draw. The
frames go from magpie to the board, so there are two ways to get one:

- **Fill a sample frame** in the preview: the shape the bridge sends, for
  playing with the drawing.
- A real one, with `node bridge/magpie-display.mjs --dry-run --once`, which
  prints exactly what it would push. The board itself answers what it last drew,
  which is the same JSON it received:

```sh
curl -s http://magpie-desk.local:9877/health
```

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
interpretation of the drawing that could drift from it. At runtime a mask is
blended in the colour of the agent it flies for.

## What a frame does not say

A display sits in a room other people walk through, so a frame names providers,
models and agents, and nothing else:

- no account, email, key name or key id;
- no session, request id or folder;
- no prompt text, reply text or error text.

The trace the bridge reads carries more than a frame does — an account in
`tries[].id`, the account that answered in `affinity`, one on each quota card,
the session and folder — and the bridge drops all of it. [The bridge's
README](bridge/README.md#privacy) has the table of what is dropped.

The token is not sent by discovery, either: a probe is broadcast, so an answer
that named the token would hand it to whoever asked. It is set on both ends by
hand.

## Verifying

```sh
# the bridge, against a running magpie: one frame, printed
node bridge/magpie-display.mjs --dry-run --once

# the baked bird is up to date
python3 tools/gen-bird.py --check

# the firmware builds for the board
cd firmware && pio run
```

The preview's JavaScript is checked the way the firmware's output is:

```sh
python3 - <<'PY'
import re; src = open("preview/flight.html").read()
open("/tmp/flight.js","w").write(re.findall(r"<script>(.*?)</script>", src, re.S)[-1])
PY
node --check /tmp/flight.js
```
