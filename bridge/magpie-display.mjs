#!/usr/bin/env node
// magpie desk display bridge.
//
// magpie's own display plugin is a built-in Go plugin: it is compiled into the
// binary, so it cannot be added to a magpie that was built without it. A JS
// gateway plugin is not an option either: those run on moejs inside the
// gateway, with no network access, so they cannot reach a board.
//
// This bridge is the way to serve a board from any magpie: it reads what the
// gateway already serves on its own local HTTP API — the routing trace, today's
// usage and the quota cards — and pushes the same v1 frames the built-in plugin
// pushes (docs/protocol.md). The firmware does not know or care which one is
// pushing.
//
//   node magpie-display.mjs --board 192.168.31.200
//   node magpie-display.mjs --discover          let the board find this
//   node magpie-display.mjs --dry-run           print frames, push nothing
//
// Nothing here is magpie-specific beyond three GETs, so it works against any
// magpie version that serves them (0.1.x).

import http from "node:http";
import dgram from "node:dgram";
import os from "node:os";

// ---------------------------------------------------------------- arguments

const args = parseArgs(process.argv.slice(2));
if (args.help) {
  console.log(`
magpie desk display bridge

  --board <host[:port]>   push to this display (default port 9877)
  --discover              answer the display's broadcast instead of a fixed board
  --token <string>        the X-Magpie-Token the display holds
  --gateway <url>         where magpie's gateway is (default http://127.0.0.1:3425)
  --name <string>         what the display calls this computer (default: hostname)
  --port <n>              the display's frame port (default 9877)
  --discover-port <n>     the display's discovery port (default 9878)
  --heartbeat <s>         longest silence (default 15)
  --coalesce <ms>         wait for a change that is not the routing (default 300)
  --dry-run               print the frames, push nothing
  --once                  push one frame and exit
  --verbose               say what it does
  --help
`);
  process.exit(0);
}

const cfg = {
  gateway: (args.gateway || "http://127.0.0.1:3425").replace(/\/$/, ""),
  token: args.token || "",
  name: args.name || os.hostname(),
  port: Number(args.port || 9877),
  discoverPort: Number(args["discover-port"] || 9878),
  heartbeat: Number(args.heartbeat || 15) * 1000,
  coalesce: Number(args.coalesce ?? 300),
  dryRun: !!args["dry-run"],
  once: !!args.once,
  verbose: !!args.verbose,
};

// The displays this bridge serves. A board is not configuration: each one
// broadcasts "MAGPIE-DISPLAY?" until somebody answers, so every board that
// probes is remembered here and every frame goes to all of them. A --board
// names one into the same map, so a fixed address and discovered ones can be
// served at once.
const targets = new Map();

// remember adds a display, or refreshes one already known. okAt is when it was
// last heard from, and it is what eviction goes by: a board that has been
// unplugged must not be pushed to for ever.
function remember(url, name = "") {
  const now = Date.now();
  const t = targets.get(url);
  if (t) {
    t.okAt = now;
    if (name) t.name = name;
    return false;
  }
  targets.set(url, { name, okAt: now, fails: 0 });
  return true;
}

if (args.board) remember(withPort(String(args.board), cfg.port), "named");

// withPort is a board's push URL: what --board names, made into the address a
// frame is POSTed to. A bare host gets the frame port and the protocol's path;
// a full URL is taken as given, so an address that already names /state is not
// given it twice.
function withPort(host, port) {
  let base = host;
  if (!/^https?:\/\//.test(base)) {
    base = base.includes(":") && !/^\[/.test(base) ? `http://${base}` : `http://${base}:${port}`;
  }
  const u = new URL(base);
  if (u.pathname === "/" || u.pathname === "") u.pathname = "/state";
  return u.toString().replace(/\/$/, "");
}

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) continue;
    const k = a.slice(2);
    const next = argv[i + 1];
    if (next === undefined || next.startsWith("--")) out[k] = true;
    else (out[k] = next), i++;
  }
  return out;
}

const say = (...a) => cfg.verbose && console.error("[display]", ...a);

// ------------------------------------------------------------------ reading

// get is one GET against the gateway's own API, with a bound: a gateway that
// stopped answering must not hold the bridge's only loop.
async function get(path, ms = 8000) {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), ms);
  try {
    const res = await fetch(cfg.gateway + path, { signal: ctl.signal });
    if (!res.ok) throw new Error(`${path}: HTTP ${res.status}`);
    return await res.json();
  } finally {
    clearTimeout(t);
  }
}

// trace is the routing, changed after seq. The gateway holds a long poll up to
// 25s, so this is asked with a short wait and a bound past it.
async function trace(after) {
  const q = after > 0 ? `?after=${after}&wait=1` : "";
  return get("/v1/magpie/trace" + q, 30000);
}

// week is the seven days the Usage page is about: its four cards and the bars
// under them, from one answer. The period token is usage.Week's own value,
// "7d" — the gateway silently answers 30d for anything it does not know
// (PeriodOf in internal/usageapi/usageapi.go), so a wrong token here is not an
// error, it is a different page.
async function week() {
  return get("/v1/magpie/usage?period=7d", 8000);
}

async function quotas() {
  return get("/v1/magpie/quotas", 8000);
}

// ------------------------------------------------------------------- frames

const NAME_MAX = 24;
const CARD_MAX = 16;
const CARDS_MAX = 3;
const FRAME_MAX = 2048;

const cut = (s, n) => {
  s = String(s ?? "");
  if (s.length <= n) return s;
  // cut on a code point boundary, never half a character
  let out = "";
  for (const ch of s) {
    if ((out + ch).length > n - 1) break;
    out += ch;
  }
  return out + "~";
};

// frameOf builds one v1 frame from what the gateway served.
//
// It names providers, models and agents and nothing else: no account, no key,
// no session, no prompt text. The trace carries an account (a login, an email)
// in tries[].id and in affinity, and the quota cards carry one in user — none of
// that reaches a board that sits in a room other people walk through.
// totalOf is what a period cost in tokens: the four kinds the ledger counts.
const totalOf = (t) =>
  (t.input || 0) + (t.output || 0) + (t.cache_read || 0) + (t.cache_write || 0);

// barsOf is the week's series as the chart draws it: a bar a day, with how much
// of it came out of the cache — the lighter body and the darker cap the window
// stacks, in this frame's smallest form.
function barsOf(report) {
  const pts = (report?.series || []).slice(-7);
  return pts.map((p) => {
    const at = p.time ? new Date(p.time) : null;
    return {
      l: at && !Number.isNaN(at.getTime()) ? String(at.getDate()) : "",
      v: totalOf(p),
      c: p.cache_read || 0,
    };
  });
}

function frameOf({ route, summary, bars, cards, seq, up }) {
  const f = { v: 1, seq, now: Math.floor(Date.now() / 1000), up };

  if (route) {
    const tries = route.tries || [];
    const last = tries[tries.length - 1] || {};
    const lastUse = (route.usage || [])[route.usage?.length - 1] || {};
    const p = route.prompt || {};
    const failed = (last.status || 0) >= 400;

    f.r = {
      id: route.id,
      agent: cut(route.agent, NAME_MAX),
      provider: cut(route.provider, NAME_MAX),
      model: cut(shortModel(route.model, route.provider), NAME_MAX),
      group: route.group ? cut(route.group.name || route.group.id || "", NAME_MAX) : "",
      try: tries.length || 1,
      tries: tries.length || 1,
      moved: tries.length > 1,
      done: !!route.done,
      status: route.status || last.status || 0,
      ms: route.ms ?? (route.done ? 0 : Date.now() - Date.parse(route.time)),
      ttft: route.ttft || 0,
      fail: failed,
    };

    f.c = {
      tok: p.tokens || (lastUse.in || 0) + (lastUse.cache_read || 0),
      win: p.window || 0,
      cache: lastUse.cache_read || 0,
      out: route.out || lastUse.out || 0,
    };
    // What the prompt is made of, as the window's own waffle draws it: one
    // colour a part. Only the token counts go out — the parts' item names are
    // the prompt's contents, which a board in a room must not show.
    const parts = {};
    for (const part of p.parts || []) {
      if (part && part.kind && part.tokens > 0) parts[part.kind] = part.tokens;
    }
    if (Object.keys(parts).length) f.c.parts = parts;
  }

  if (summary) {
    // the window's Usage cards, field for field: tokens with its in/out split,
    // cache reads, reasoning, and the calls with how many failed. They used to
    // be summed into one tok here, which is why the board's page could only say
    // "2.4M tokens" where the window says what the tokens were.
    const u = {
      req: summary.calls || 0,
      tok: totalOf(summary),
      in: summary.input || 0,
      out: summary.output || 0,
      cr: summary.cache_read || 0,
      rs: summary.reasoning || 0,
    };
    if (summary.errors) u.err = summary.errors;
    // a cost of 0 with unpriced calls in it is not "free": it is unknown, and
    // the board shows no money rather than $0.00
    if (summary.cost > 0 || (summary.calls > 0 && !summary.unpriced)) u.cost = summary.cost;
    f.u = u;
  }

  if (bars && bars.length) f.s = bars;

  if (cards && cards.length) {
    f.q = cards.slice(0, CARDS_MAX).map((c) => ({
      n: cut(c.n, CARD_MAX),
      p: cut(c.p, CARD_MAX),
      d: cut(c.d, CARD_MAX),
      left: c.left,
      at: c.at,
    }));
  }
  return f;
}

// shortModel is the model's id without the provider's prefix: the frame has
// them in separate fields, and the board's header prints "provider/model".
function shortModel(model, provider) {
  model = String(model || "");
  if (provider && model.startsWith(provider + "/")) return model.slice(provider.length + 1);
  return model;
}

// cardsOf is the tightest few windows of the quota report. The card's own
// user/account is never read.
// fmt is a count a 320px screen can read: the window's own cards write "46M",
// not "46000000", and "0 / 3000000" on a quota card is the same problem.
const fmt = (n) => {
  n = Number(n) || 0;
  if (n >= 1e9) return (n / 1e9).toFixed(1) + "B";
  if (n >= 1e7) return Math.round(n / 1e6) + "M";
  if (n >= 1e6) return (n / 1e6).toFixed(1) + "M";
  if (n >= 1e3) return Math.round(n / 1e3) + "k";
  return String(Math.round(n * 10) / 10);
};

function cardsOf(report, now = Date.now()) {
  const out = [];
  for (const q of report?.data || []) {
    for (const w of q.windows || []) {
      if (w.remaining === undefined || w.remaining === null) continue;
      const left = Math.max(0, Math.min(100, Math.round(w.remaining)));
      const at = w.resetsAt ? Math.max(0, Math.round((Date.parse(w.resetsAt) - now) / 1000)) : 0;
      // the window's name alone is not an identity: two subscriptions both
      // name a window "Credits", and a card that says only that says nothing
      // about which pool it is. The provider is who serves it; the display is
      // what the window counts, which the page shows instead of a bare
      // percentage when the gateway knows one.
      let d = "";
      if (typeof w.limit === "number") d = `${fmt(w.used)} / ${fmt(w.limit)}`;
      else if (w.display) d = String(w.display);
      out.push({
        n: w.name || q.plan || q.name || "",
        p: q.name || q.provider || "",
        d,
        left,
        at,
      });
    }
  }
  // the tightest first, so the three a board can show are the three worth
  // showing
  out.sort((a, b) => a.left - b.left || a.at - b.at);
  return out;
}

function encode(f) {
  let body = JSON.stringify(f);
  if (Buffer.byteLength(body) <= FRAME_MAX) return body;
  // the quota cards are the largest part and the least urgent
  if (f.q) {
    delete f.q;
    body = JSON.stringify(f);
    if (Buffer.byteLength(body) <= FRAME_MAX) return body;
  }
  // then the names, which are the rest of the size and all the text
  for (const k of ["agent", "provider", "model", "group"]) {
    if (f.r) f.r[k] = cut(f.r[k], 12);
  }
  return JSON.stringify(f);
}

// ------------------------------------------------------------------ pushing

// seq numbers the frames. The board drops a frame whose seq is not above the
// last it took (that is how a reordered or repeated one is ignored), so a
// counter that started at 1 would be dropped by a board that had ever seen a
// higher one — every restart of this bridge would go silent until the board was
// power-cycled. Milliseconds since the epoch are monotonic across restarts and
// still increase by one per frame within a run.
let seq = Date.now();
let lastRouteKey = "";
let pushed = 0;

async function push(url, f) {
  const body = encode(f);
  if (cfg.dryRun) {
    console.log(body);
    return true;
  }
  if (!url) return false;
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), 1200);  // pushes run in parallel,
    // so a slow board costs its own second and not the others'
  try {
    const res = await fetch(url, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        ...(cfg.token ? { "X-Magpie-Token": cfg.token } : {}),
      },
      body,
      signal: ctl.signal,
    });
    if (!res.ok) {
      const why = (await res.text().catch(() => "")).slice(0, 120);
      console.error(`[display] ${url} answered ${res.status}${why ? ": " + why : ""}`);
      return false;
    }
    pushed++;
    say(`pushed #${f.seq} (${body.length}B) seq=${f.seq}`);
    return true;
  } catch (e) {
    console.error(`[display] push failed: ${e.message}`);
    return false;
  } finally {
    clearTimeout(t);
  }
}

// routeKey is what makes a push immediate: a new request, another try, or the
// end of one. The tokens of the same try are not — those coalesce.
function routeKeyOf(r) {
  if (!r) return "";
  const tries = r.tries || [];
  const last = tries[tries.length - 1] || {};
  return `${r.id}|${tries.length}|${r.done ? 1 : 0}|${last.status || 0}`;
}

// ----------------------------------------------------------------- the loop

let summary = null;
let bars = null;
let cards = [];
let slowAt = 0;
let lastPushAt = 0;
let pending = null;
let lastSeq = 0;
let current = null;

async function refreshSlow(now) {
  if (now - slowAt < 5000) return;
  slowAt = now;
  try {
    const r = await week();
    summary = r;          // the four cards and the chart are one period now
    bars = barsOf(r);
  } catch (e) {
    say("usage:", e.message);
  }
  try {
    cards = cardsOf(await quotas(), now);
  } catch (e) {
    say("quotas:", e.message);
  }
}

async function send(reason) {
  const now = Date.now();
  await refreshSlow(now);
  const f = frameOf({ route: current, summary, bars, cards, seq: ++seq, up: 1 });
  // A dry run has no board to need: it is how the mapping is checked against a
  // gateway before anything is on the network.
  if (cfg.dryRun) {
    lastRouteKey = routeKeyOf(current);
    say("sent:", reason);
    return;
  }
  const urls = [...targets.keys()];
  if (!urls.length) {
    say("no board yet");
    return;
  }
  // One frame, every display, all at once. Sequentially, a board that has been
  // unplugged would hold up the ones that have not for the length of its
  // timeout on every single push.
  const settled = await Promise.allSettled(urls.map((u) => push(u, f)));
  let any = false;
  urls.forEach((u, i) => {
    const t = targets.get(u);
    if (!t) return;
    if (settled[i].status === "fulfilled" && settled[i].value) {
      t.okAt = now;
      t.fails = 0;
      any = true;
      return;
    }
    t.fails++;
    // Five missed heartbeats is a board that has gone away, not a slow one:
    // drop it, and it is back the moment it probes again.
    if (t.fails >= 5 || now - t.okAt > 120000) {
      targets.delete(u);
      say("dropped:", u);
    }
  });
  if (any) lastPushAt = now;
  lastRouteKey = routeKeyOf(current);
  say("sent:", reason, any ? "" : "(no display took it)");
}

function schedule(ms, reason) {
  if (pending) return; // the one already pending is the newest state anyway
  pending = setTimeout(() => {
    pending = null;
    send(reason).catch((e) => console.error("[display]", e.message));
  }, ms);
}

// boardSeq is the seq of the frame the board holds, from its own /health: a
// push has to be above it to be drawn.
async function boardSeq() {
  if (cfg.dryRun) return 0;
  let max = 0;
  for (const url of targets.keys()) {
    const base = url.replace(/\/state$/, "");
    const ctl = new AbortController();
    const t = setTimeout(() => ctl.abort(), 3000);
    try {
      const res = await fetch(base + "/health", { signal: ctl.signal });
      if (res.ok) {
        const n = Number((await res.json()).seq) || 0;
        if (n > max) max = n;
      }
    } catch {
      // a display that does not answer is one eviction will deal with
    } finally {
      clearTimeout(t);
    }
  }
  return max;
}

// newestRoute reads the routing and keeps the newest request: the one the
// board shows. A one-shot frame and the loop both start from it.
async function newestRoute() {
  const st = await trace(lastSeq);
  lastSeq = st.seq || lastSeq;
  const routes = st.routes || [];
  if (routes.length) {
    current = routes.reduce((a, b) => (!a || (b.id || 0) >= (a.id || 0) ? b : a), null);
  }
  return current;
}

async function loop() {
  // the first frame is asked for at once, so a board that just came up draws
  // something real rather than an empty stage
  await send("start");

  for (;;) {
    const beat = setTimeout(() => {
      send("heartbeat").catch(() => {});
    }, cfg.heartbeat);

    try {
      const st = await trace(lastSeq);
      lastSeq = st.seq || lastSeq;
      const routes = st.routes || [];
      if (routes.length) {
        // the newest route is the one the board shows
        current = routes.reduce((a, b) => (!a || (b.id || 0) >= (a.id || 0) ? b : a), null);
        const key = routeKeyOf(current);
        if (key !== lastRouteKey) {
          // the routing changed: at once, not coalesced
          if (pending) clearTimeout(pending), (pending = null);
          await send("routing");
        } else {
          schedule(cfg.coalesce, "coalesced");
        }
      } else if (Date.now() - lastPushAt > cfg.heartbeat) {
        await send("heartbeat");
      }
    } catch (e) {
      console.error(`[display] ${e.message}`);
      // a gateway that isn't there is not a reason to spin
      await new Promise((r) => setTimeout(r, 2000));
    } finally {
      clearTimeout(beat);
    }
  }
}

// -------------------------------------------------------------- discovery

// The board broadcasts "MAGPIE-DISPLAY?" and takes the unicast answer as where
// to push. The answer carries no token: a probe is broadcast, so naming the
// token would hand it to whoever asked. Both ends are given it by hand.
function discover() {
  const sock = dgram.createSocket({ type: "udp4", reuseAddr: true });
  sock.on("error", (e) => console.error("[display] discovery:", e.message));
  sock.on("message", (buf, rinfo) => {
    if (buf.toString() !== "MAGPIE-DISPLAY?") return;
    // answer a private or link-local source only: a bridge on a public host
    // would otherwise be a reflector
    if (!isLan(rinfo.address)) return;
    const push = `http://${rinfo.address}:${cfg.port}/state`;
    const body = JSON.stringify({
      v: 1,
      object: "magpie.display",
      push,
      name: cfg.name,
      auth: !!cfg.token,
    });
    sock.send(body, rinfo.port, rinfo.address, (err) => {
      if (err) return;
      if (remember(push, "discovered")) say("board found:", push);
    });
  });
  sock.bind(cfg.discoverPort, () => say(`discovery on udp ${cfg.discoverPort}`));
}

function isLan(ip) {
  return (
    /^10\./.test(ip) ||
    /^192\.168\./.test(ip) ||
    /^172\.(1[6-9]|2\d|3[01])\./.test(ip) ||
    /^169\.254\./.test(ip) ||
    ip === "127.0.0.1"
  );
}

// ------------------------------------------------------------------- start

(async () => {
  say(`gateway ${cfg.gateway} → ${targets.size ? [...targets.keys()].join(" ") : "(discovery)"}`);
  if (args.discover) discover();
  if (cfg.once) {
    // a board probes on its own clock, so a one-shot frame gives one a moment
    // to arrive rather than reporting that nobody is there
    if (args.discover && !targets.size) await new Promise((r) => setTimeout(r, 3000));
    // read the routing first: a one-shot frame with no route in it would say
    // nothing about the mapping, which is what a dry run is for
    try {
      await newestRoute();
    } catch (e) {
      console.error("[display]", e.message);
    }
    // and start above what the board already took, so the frame is not dropped
    // as one it has seen
    try {
      seq = Math.max(seq, await boardSeq());
    } catch (e) {
      say("board seq:", e.message);
    }
    await send("once");
    process.exit(cfg.dryRun || pushed ? 0 : 1);
  }
  loop();
})().catch((e) => {
  console.error("[display]", e.message);
  process.exit(1);
});
