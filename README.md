# Magpie Buddy

**English** · [简体中文](README.zh.md)

A small screen on the desk that shows what magpie is doing: which agent asked,
which model answered, how full the context window is, and what is left on each
allowance. When a request goes out, a magpie flies along the route.

<img src="magpie-buddy.png" alt="Magpie Buddy on an M5Stack CoreS3, showing the Route page" width="620">

**Hardware:** M5Stack CoreS3 (ESP32-S3, 320×240, 8 MB PSRAM, Wi-Fi).

On a [StackChan](https://docs.m5stack.com/en/stackchan) body the twelve RGB
lights under the head follow the same states: the agent's hue breathing while a
request is out, mint when an answer lands, red when a try fails. A bare CoreS3
has no such lights and the firmware simply says so — nothing to configure.

## Run it

**1. The bridge** — on the machine running magpie:

```sh
node bridge/magpie-display.mjs --discover
```

No install, no build, no dependencies. It reads three GETs magpie's gateway
already answers and pushes frames to the board.

**2. The firmware** — once, over USB-C:

Open **[the flasher](https://nullbeing.github.io/magpie-buddy/flasher/)** in
Chrome or Edge and press Connect. No toolchain needed. From a checkout instead:

```sh
cd firmware
pio run -t upload
```

**3. The network** — from a phone, no rebuild:

The board raises an access point called `Magpie-Buddy-XXXX` and shows the name
on its screen. Join it; the setup page opens by itself (or go to
`http://192.168.4.1`). Pick your network, type the password, and the board
saves it to NVS and restarts onto it.

Hold the screen for three seconds to reach that page again. The saved network is
kept — tap once to go back. To change it, open the page and press **Forget**
under "Current network": the board stays where it is, still an access point,
so the next network can be picked without a reboot dropping the page mid-tap.

## The pages

| Page | What it shows |
| --- | --- |
| Route | which agent asked, which model answered, and the flight between them |
| Context | the context window as magpie's own waffle, by part |
| Usage | a week of tokens, cache and calls, as the window's four cards and chart |
| Limits | what is left on each allowance, tightest first |

Swipe to move between them, tap the tabs to jump, tap the sun or moon to switch
theme. Dark and light are both there.

## Why a bridge and not a plugin

magpie's plugins cannot do this. A built-in plugin is compiled into magpie, so
shipping one means forking it. The JS plugin runtime has no network at all — no
`fetch`, no sockets — so it can never reach a board. The bridge sits outside
magpie, which is the only place with both a network and the state.

It touches no magpie source: three GETs the gateway already answers.

## Docs

| | |
| --- | --- |
| [docs/protocol.md](docs/protocol.md) | the wire protocol: discovery, frames, timing, and what a frame deliberately does not say |
| [docs/design.md](docs/design.md) | how it is built: the preview and the renderer, the baked atlases, the flight's timings, privacy |
| [bridge/README.md](bridge/README.md) | the bridge's flags, and what it drops from the trace |

## Checking it

```sh
node bridge/magpie-display.mjs --dry-run --once   # one frame, printed
make -C tools/render                              # the screen, without the board
cd firmware && pio run                            # the firmware builds
```
