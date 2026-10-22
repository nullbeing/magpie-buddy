# magpie desk display bridge

Serves a Magpie Buddy board from **any** magpie, including a release
build that has no desk-display plugin in it.

```sh
node magpie-display.mjs --discover
```

That is the whole setup. The board broadcasts, the bridge answers, the frames
start.

## Why a bridge and not a plugin

magpie has two kinds of plugin, and neither can do this:

| Kind | Installed with | Why not |
| --- | --- | --- |
| Built-in plugin (`plugins/display` in magpie's source) | nothing — it is compiled into the binary | Cannot be added to a binary built without it. A release build has no way to load one. |
| JS gateway plugin | `magpie plugin add ./x.mjs` | Runs on moejs *inside* the gateway, with no network access: no `fetch`, no sockets. It can change requests; it cannot reach a board. |

So the bridge is a separate process. It reads what the gateway already serves on
its own local HTTP API and pushes the same v1 frames the built-in plugin pushes
(`../docs/protocol.md`). **The firmware does not know or care which one is
pushing it.**

It needs nothing but Node: no `npm install`, no dependencies, no build step.

## Running it

```sh
node magpie-display.mjs --discover                    # every board that probes
node magpie-display.mjs --board 192.168.31.200        # a fixed address, no probing
node magpie-display.mjs --dry-run --once              # print one frame, push nothing
```

| Flag | What it does |
| --- | --- |
| `--board <host[:port]>` | Push to this display as well as to any that are discovered. A bare host gets port 9877 and `/state`. |
| `--discover` | Answer every display's UDP broadcast. Each board that probes is added to the set this bridge pushes to, so one bridge drives as many as are on the network. |
| `--token <string>` | The `X-Magpie-Token` the display holds (its `DISPLAY_TOKEN`). |
| `--gateway <url>` | Where magpie is (default `http://127.0.0.1:3425`). |
| `--name <string>` | What the board calls this computer (default: hostname). |
| `--heartbeat <s>` | Longest silence (default 15). A display that stops taking frames is dropped after five in a row, and returns when it next probes. |
| `--coalesce <ms>` | Wait for a change that isn't the routing (default 300). |
| `--dry-run` | Print the frame, push nothing. **Use this first.** |
| `--once` | One frame, then exit. |
| `--verbose` | Say what it is doing. |

### Checking it before a board is involved

`--dry-run --once` prints exactly what the board would receive, built from your
gateway's live data:

```sh
$ node magpie-display.mjs --dry-run --once | python3 -m json.tool
{
  "v": 1, "seq": 1792556356882, "now": 1760000000, "up": 1,
  "r": { "id": 1792463946850, "agent": "pi", "provider": "workbuddy-ai",
         "model": "deepseek-v4.1-flash", "try": 1, "tries": 1,
         "done": true, "status": 200, "ms": 8409, "fail": false },
  "c": { "tok": 436216, "win": 1000000, "cache": 435328, "out": 986 },
  "u": { "req": 385, "tok": 111817030, "err": 1, "cost": 0 },
  "q": [ { "n": "Chat requests", "left": 19, "at": 934843 }, … ]
}
```

A frame is 400–500 bytes, never over 2048.

## What it reads

Three GETs against the gateway's own API, all of which a stock magpie serves:

| Endpoint | What it gives the frame |
| --- | --- |
| `/v1/magpie/trace?after=<seq>&wait=1` | the routing: agent, provider, model, tries, status, ms, ttft, and `prompt.tokens` / `prompt.window` |
| `/v1/magpie/usage?period=today` | today: calls, tokens, errors, cost |
| `/v1/magpie/quotas` | the allowance cards: name, remaining %, reset |

If the gateway is on another machine, name it: `--gateway http://other:3425`.
Those routes answer another machine only when magpie is shared on the local
network with an enabled gateway key, so on one machine they are loopback-only.

## Privacy

The frame names providers, models and agents, and nothing else. The trace
carries more than that, and the bridge drops it:

| In the trace | In the frame |
| --- | --- |
| `tries[].id` — an account, an email or a login | the provider only |
| `affinity.last` — the account that answered | not read |
| `quotas[].user` — the account | not read |
| `session`, `folder` | not read |
| `prompt.parts` — the prompt's text and item names | only `tokens` and `window` |

A board sits in a room other people walk through. This is the same allow-list
magpie itself uses for a JS plugin's `onUsage`.

The token is never sent over the network either: discovery's answer carries
`auth: true/false`, never the secret. Both ends are given it by hand.

## Timing

| Trigger | Delay |
| --- | --- |
| a new request, another try, or the end of one | at once |
| anything else | coalesced, ≤ 300 ms |
| nothing changed | 15 s heartbeat |

Coalescing is latest-wins, not a queue: a board that is asleep or gone costs
nothing, and a frame that is already out of date is replaced rather than sent.

## Keeping it running

The bridge is a plain foreground process. For a board on a desk, run it under
something that restarts it:

```sh
# macOS: ~/Library/LaunchAgents/ai.usemagpie.display.plist
#   ProgramArguments: ["/usr/local/bin/node", "/path/to/magpie-display.mjs", "--discover"]
#   RunAtLoad: true, KeepAlive: true

# systemd --user
#   ExecStart=/usr/bin/node /path/to/magpie-display.mjs --discover
#   Restart=always
```

The frame's `seq` is a millisecond timestamp, so a restart never goes silent: a
board drops a frame whose `seq` is not above the last it took, and a counter
starting at 1 would be dropped by any board that had seen a higher one.

## Verifying against the board

```sh
curl -s http://192.168.31.200:9877/health
{"v":1,"object":"magpie.display.device","seq":1792555345473,"uptime":217,"rssi":-49,"magpie":"Desk"}
```

`seq` is the frame the board holds; `magpie` is the name the bridge sent. If
`seq` stays 0, no frame has been accepted — check the token, and check that the
board is on the same network.

## Troubleshooting

**The board answers on 9877 but takes 10–20 s to reply.** Its Wi-Fi modem sleep
is on. The firmware turns it off after the join; if you built the firmware
yourself before that fix, rebuild and reflash. Measured symptom: 671 ms RTT and
60 % loss to a board 2 ms from its router, while the router itself lost nothing.

**`--discover` finds nothing.** The board broadcasts to `255.255.255.255` and to
its own subnet's broadcast address, on UDP 9878. Check the board is joined (its
screen says `magpie offline` until the first frame), and that no firewall drops
UDP 9878.

**`404` on push.** The URL is missing `/state`. `--board` adds it; a full URL is
taken as given.

**`401` on push.** The board has a `DISPLAY_TOKEN` and the bridge was not given
`--token`, or the two differ.
