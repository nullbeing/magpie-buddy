# Wire protocol, v1

Two halves, and they never both listen: the display **probes** once and then
only listens, and magpie **listens** once and then only pushes. Nothing polls
on a timer, so an idle display costs one UDP broadcast every 30 s and one TCP
POST every 15 s.

```
   display                                    the pusher (bridge/ or magpie's own plugin)
      │                                              │
      │  1. UDP broadcast :9878  "MAGPIE-DISPLAY?"    │
      ├─────────────────────────────────────────────►│
      │                                              │
      │  2. UDP unicast  :9878  {push, name, auth}    │
      │◄─────────────────────────────────────────────┤
      │                                              │
      │  3. POST http://<display>:9877/state          │
      │◄─────────────────────────────────────────────┤  on a route change,
      │      (X-Magpie-Token)                        │  else ≤ 300 ms coalesced,
      │                                              │  else a 15 s heartbeat
      │  200 OK                                       │
      ├─────────────────────────────────────────────►│
```

## Ports

| Port | Proto | Who listens | What |
| --- | --- | --- | --- |
| 9877 | TCP | display | `POST /state` (frames), `GET /health` (what it last drew) |
| 9878 | UDP | magpie | the discovery probe and its answer |

Neither is fixed by the firmware if it disagrees; the pusher's `discoverPort`
and the display's own `DISCOVERY_PORT` are both settings. The ports are in the
9800s so they don't collide with the gateway's 3425 or the dev ports 3426–3429.

## 1 · Discovery

The display broadcasts a datagram of exactly 15 bytes to
`255.255.255.255:9878`, and to the subnet broadcast address of its own
interface when the router drops the all-ones address (many do):

```
MAGPIE-DISPLAY?
```

It repeats every 30 s until it has received a frame in the last 60 s, and
again after 3 missed heartbeats. It also repeats once after every boot and
after every Wi-Fi reconnect.

magpie answers the sender by unicast, and remembers the source address:

```json
{"v":1,"object":"magpie.display","push":"http://192.168.1.42:9877/state","name":"studio-mac","auth":true}
```

- `push` is where the frames go: `http://<display-ip>:9877/state`, built from
  the probe's source address and the display's port.
- `name` is this computer's host name, for the display's status line.
- `auth` says the frames will carry a token, so a board with none configured
  can say so on its own screen instead of being refused in the dark.

**The answer carries no token, on purpose.** A probe is broadcast, so anyone on
the network can send one; an answer that named the token would hand it to
whoever asked, and the token would stop being a secret the moment discovery
worked at all. The token is set by hand on both ends — the pusher's `--token`
and the board's `DISPLAY_TOKEN` in `config.h` — and discovery only says where to
push. Two values that disagree show up as `401` on every push, which
the board says on screen.

A pusher given a fixed address (`--board`) skips discovery entirely and pushes
there.

Discovery itself is opt-in on the pusher's side: the bridge answers a probe only
with `--discover`. A machine that has never heard of a desk display opens no
socket. It also answers only a private or
link-local source: magpie can sit in Docker on a public host, where answering
any address at all would make it a UDP reflector.

## 2 · Frames

`POST /state`, `Content-Type: application/json`, `X-Magpie-Token: <token>`.
The body is one whole snapshot, never a delta: the display holds no state it
did not get from the last frame, so a dropped frame, a reboot or a
re-flash recovers on the next one.

The display answers `200` with an empty body. Any other status, or no answer
within 2 s, is a failed push: magpie counts it, keeps the previous frame as
still current, and does not queue frames up — the next change replaces the one
that failed.

### Shape

```jsonc
{
  "v": 1,
  "seq": 4211,            // monotonic; the display drops a frame it already has
  "now": 1760000000,      // unix seconds, magpie's clock
  "up": 1,                // 1 while magpie serves the gateway, 0 when it doesn't

  "r": {                  // the newest route, absent before the first request
    "id": 4211,           // route id; a new id is a new flight
    "agent": "claude",    // magpie's agent id
    "provider": "kimi",
    "model": "kimi-k2.5",
    "group": "opus-anywhere",   // "" when the agent asked for a model, not a group
    "try": 2,             // which try this is, 1-based
    "tries": 3,           // how many the routing offered
    "moved": true,        // a try before this one failed: the bird goes back to the hub
    "done": false,        // the request has ended
    "status": 0,          // HTTP status when done; 0 while in flight
    "ms": 1840,           // so far, or the total when done
    "ttft": 620,          // ms to the first content; 0 when not streamed yet
    "fail": false         // this try failed and handed the request on
  },

  "c": {                  // the context window of the model that answered
    "tok": 48213,         // the prompt, as the vendor counted it
    "win": 200000,        // the model's window; 0 when not known
    "cache": 44000,       // of tok, read from the vendor's cache
    "out": 1204,          // the reply's tokens
    // parts is what the prompt is made of, by magpie's own names for the
    // parts (PARTS in internal/gui/assets/context.js). The CONTEXT page draws
    // it as the window's waffle: one colour a part. A frame that names none
    // leaves it out, and the page then shows the used/free split without
    // colours rather than inventing a breakdown.
    "parts": {"system": 6000, "tools": 5200, "files": 12000,
              "results": 14000, "chat": 11000}
  },

  "u": {                  // today, from the usage ledger
    "req": 37,            // calls
    "tok": 1240000,       // the total, the four kinds summed
    "in": 1000000,        // the prompt's own tokens
    "out": 240000,        // the reply's
    "cr": 1200000,        // served from the vendor's cache, so the page can
                          // show a hit rate: cr / (cr + in)
    "rs": 90000,          // reasoning, billed as output and counted apart
    "err": 1,
    "cost": 2.41          // USD; null when no price is known
  },

  "s": [                  // the week's chart, oldest first, up to seven
    {"l": "15", "v": 8200000, "c": 6100000},   // l: day of the month
    {"l": "16", "v": 14000000, "c": 12000000}  // v: tokens, c: of them cached
  ],

  "q": [                  // up to 3 cards, the tightest window first
    {"n": "Chat requests", "p": "GitHub Copilot", "d": "161.7 / 200",
     "left": 19, "at": 934843},
    {"n": "30 days", "p": "OpenAI", "d": "41 / 250",
     "left": 18, "at": 172800}
    // n: the window's own name; p: who serves it, which is what tells two
    // subscriptions both naming a window "Credits" apart; d: what the window
    // counts, when the gateway knows one; at: seconds until reset, 0 none
  ],
}
```

### What is deliberately not here

The frame names providers, models and agents, and nothing else about them:

- no `ProviderAccount` (an email, a login), no `ProviderKeyID`, no key names;
- no `Session`, no `RouteID` beyond the flight id, no request id;
- no prompt text, no reply text, no error text, no folder, no URL;
- no cost per call, only today's total.

This is the same allow-list `internal/middleware/points.go` uses for `onUsage`
(`usageRow`), which exists because a field's *name* is not a privacy policy.
A display sits in a room other people walk through.

### Field limits

The display's parser is fixed-size, so magpie truncates:

| Field | Limit |
| --- | --- |
| `agent`, `provider`, `model`, `group` | 24 bytes, cut on a rune boundary |
| `q[].n`, `q[].p`, `q[].d` | 16 bytes |
| `q` | 3 cards |
| `s` | 7 bars |
| `s[].l` | 3 bytes |
| the whole body | 2048 bytes |

A frame over 2048 bytes is a bug in the pusher, not a reason to send more:
the display answers `413` and magpie logs it.

## 3 · Timing

| Trigger | Delay |
| --- | --- |
| a route's `id`, `try`, `done` or `status` changed | at once |
| anything else (tokens, context, usage, quotas) | coalesced, ≤ 300 ms |
| nothing changed | 15 s heartbeat |

Coalescing is latest-wins, not a queue: the pusher keeps one pending frame and
replaces it. A display that is slow, asleep or gone costs the gateway nothing
and never makes a request wait — the observer only ever hands a frame to a
channel of one.

A heartbeat keeps the display's `up` true and its clock right, and lets it tell
a quiet magpie from a dead one: three missed heartbeats (45 s) and the display
shows `offline` on its own, without magpie having to say goodbye.

## 4 · Versions

`v` is the frame version. The display accepts the highest `v` it knows and
ignores unknown keys, so a newer magpie may add fields; a display that sees a
`v` above its own says so on screen and draws what it understands. Removing or
re-typing a key is a new `v`.
