// The screen: three pages, and the flight that plays when a request arrives.
//
// 320x240 is small, so a page holds one idea in large type rather than three in
// small. The pages are switched by swiping, and a tap on a tab at the foot
// jumps straight to one.
//
//   ROUTE    which agent asked, which account answered, and how it got there
//   CONTEXT  what the prompt is made of, as the window's own waffle
//   USAGE    today's calls and tokens, and what is left on each allowance
//
// A swipe draws both pages at once — the one leaving and the one arriving, each
// pushed at an offset — so the page follows the finger and the next comes in
// from the side. The release finishes the move or puts it back, eased. The tabs
// are drawn straight to the panel, so they never slide.
//
// The look is flat: a near-black ground, one raised surface for a box that has
// to hold something, hairline rules instead of borders, and type that carries
// the hierarchy.
//
// The text is Inter, drawn from the atlases in ui_font.h — not M5GFX's fonts,
// which are 1-bit and turn every curve into a staircase. tools/render compiles
// this same file, so the preview shows the board's own text.
//
// The flight is magpie's Routing page's choreography, step for step, at its own
// timings. Every step is driven from loop() against millis(), never from
// delay(): the touch panel has to keep working while a bird is in air.
#include "screen.h"

#include <math.h>

#include "bird.h"
#include "icons.h"
#include "ui_text.h"

namespace magpie {
namespace {

// The dot matrix, as a proposal switch, because the rule it tests is not
// obvious until it is on the glass:
//
//   0  Inter only — the page as it was
//   1  dots at the sizes already in use: 18, 26 and the 34px hero
//   2  dots everywhere, 11 and 14 included
//   3  dots for the hero only, and the hero grows to 44 to make room for them
//
// Mode 1 is the experiment that fails: at 34px Doto's dots are two pixels wide
// and the face reads as noise rather than as a matrix. Mode 2 fails worse — at
// 11px a dot matrix stops being a texture and becomes a legibility problem.
// Mode 3 is the one that works, and the reason is the size, not the typeface: a
// dot matrix needs room for the dots. It is why Nothing sets NDot at display
// sizes only, and why the hero has to grow before the dots can appear at all.
// The faces the pages draw with: one per role, all of them Inter, baked by
// tools/gen-font.py. The names stay because the layout maths reads them.
#define UI_11 ui::ui_11
#define UI_14 ui::ui_14
#define UI_18 ui::ui_18
#define UI_26 ui::ui_26
#define UI_34 ui::ui_34

// Where the line under the hero starts.
#define HERO_H 44

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The palette, in two, because the window has two and a desk display sits in
// both a lit room and a dark one. Every RGB565 value is computed from an sRGB
// one, so a constant here is never a guess.
//
// The dark set is the board's own: darker than the window's dark theme, because
// a small screen held at arm's length wants more separation than a browser
// window does. The light set is the window's light theme as app.css states it
// (--bg #f4f4f6, --card #ffffff, --line #e3e3e8, --fg #1c1c21, --red #d64545,
// --green #189a58, --amber #b45309), which is why the two themes are the same
// product rather than two designs.
struct Palette {
  uint16_t bg, surface, surface2, hairline;
  uint16_t fg, fg2, fg3;
  // an empty cell: a waffle square the prompt never reached, a meter's unfilled
  // blocks. Its own value, because the one that worked on the dark ground
  // (#1E2228 against #0A0B0D) is invisible on the light one.
  uint16_t track;
  uint16_t chart, chartCap;
  uint16_t danger, warn, ok;
  uint16_t part[kPartCount];
  bool isDark;
};

constexpr Palette kPaletteDark = {
    /* bg        */ 0x0841,  // #0A0B0D
    /* surface   */ 0x10C3,  // #15181C
    /* surface2  */ 0x1905,  // #1E2228
    /* hairline  */ 0x2146,  // #262B32
    /* fg        */ 0xF79E,  // #F0F2F5
    /* fg2       */ 0x9D15,  // #98A0AC
    /* fg3       */ 0x6BB0,  // #6B7480
    /* track     */ 0x1905,  // #1E2228
    /* chart     */ 0x7BBC,  // #7d74e0
    /* chartCap  */ 0xC63E,  // #c7c4f5
    /* danger    */ 0xE28A,  // #E05252
    /* warn      */ 0xDD07,  // #D9A03C
    /* ok        */ 0x3EF2,  // #3ADD94
    /* part      */ {0x7B7A, 0x4CD8, 0xCCC7, 0x3D71, 0xC352, 0x6BFA},
    /* isDark    */ true,
};

// The parts keep their hues but darken: the window's own six are mid-tones
// chosen to sit on a dark ground, and on white they would wash out.
constexpr Palette kPaletteLight = {
    /* bg        */ 0xF7BE,  // #f4f4f6
    /* surface   */ 0xFFFF,  // #ffffff
    /* surface2  */ 0xFFDF,  // #f9f9fb
    /* hairline  */ 0xE71D,  // #e3e3e8
    /* fg        */ 0x18E4,  // #1c1c21
    /* fg2       */ 0x4A4A,  // #4b4b53
    /* fg3       */ 0x8431,  // #85858d
    /* track     */ 0xE71D,  // #e3e3e8 — the window's --line
    /* chart     */ 0xC63E,  // #c7c4f5
    /* chartCap  */ 0x5A7A,  // #5b4fd6
    /* danger    */ 0xD228,  // #d64545
    /* warn      */ 0xB281,  // #b45309
    /* ok        */ 0x1CCB,  // #189a58
    /* part      */ {0x5A77, 0x2B92, 0x9B62, 0x1C4C, 0xA24E, 0x4AF7},
    /* isDark    */ false,
};

Palette gPal = kPaletteDark;

// The page reads the palette through these names. They were constant before
// the light theme existed; keeping the names means the theme change touched one
// declaration instead of the seventy-odd places that draw with them.
#define kBg (gPal.bg)
#define kSurface (gPal.surface)
#define kSurface2 (gPal.surface2)
#define kHairline (gPal.hairline)
#define kFg (gPal.fg)
#define kFg2 (gPal.fg2)
#define kFg3 (gPal.fg3)
#define kTrack (gPal.track)
#define kChart (gPal.chart)
#define kChartCap (gPal.chartCap)
#define kDanger (gPal.danger)
#define kWarn (gPal.warn)
#define kOk (gPal.ok)
#define kPartColor (gPal.part)
// A hue picked against a dark ground has to be taken down to sit on white.
const char* const kPartName[kPartCount] = {"system", "tools", "memory",
                                           "files", "results", "chat"};

// An agent's colour: its maker's, else one of the spares by its id — never red,
// which on this screen means a failure.
struct Hue {
  const char* id;
  uint16_t c;
};
constexpr Hue kHues[] = {
    {"claude", 0xDBAE}, {"codex", 0x633C}, {"gemini", 0x0D3C}, {"copilot", 0xA9F5},
    {"cursor", 0x15D4}, {"opencode", 0xE98E}, {"crush", 0xEA7A}, {"goose", 0x85E3},
    {"pi", 0x10DE}, {"omp", 0xD98F}, {"omo", 0x7C3D}, {"dsh", 0x061A},
    {"commandcode", 0xF8E0}, {"mimocode", 0x3B3E}, {"deveco", 0x3CB9},
};
constexpr uint16_t kSpares[] = {0x8B5F, 0x22E6, 0x0DAE, 0x08B8, 0xCB40, 0xDB8C, 0x25A5, 0x6591};

// darken takes a colour towards black. The agent hues were chosen against a
// dark ground; on white the same mid-tones are a smudge, so the light theme
// takes them down before they are drawn.
uint16_t darken(uint16_t c, int pct) {
  const int r = ((c >> 11) & 0x1F) * pct / 100;
  const int g = ((c >> 5) & 0x3F) * pct / 100;
  const int b = (c & 0x1F) * pct / 100;
  return uint16_t((r << 11) | (g << 5) | b);
}

uint16_t hueOf(const char* id) {
  uint16_t c = 0;
  bool found = false;
  for (const Hue& h : kHues) {
    if (strcmp(h.id, id) == 0) {
      c = h.c;
      found = true;
      break;
    }
  }
  if (!found) {
    uint32_t h = 0;
    for (const char* p = id; p && *p; p++) h = (h * 31 + static_cast<uint8_t>(*p)) & 0xFFFFFFFF;
    c = kSpares[h % (sizeof kSpares / sizeof kSpares[0])];
  }
  return gPal.isDark ? c : darken(c, 58);
}

// kfmt is "48k", "1.2M": a number a 320-pixel screen can show.
void kfmt(char* out, size_t cap, long long n) {
  // A tenth of a million is noise once the number is big, and the tenth is what
  // pushed "743.0M" past the width of a cell. Past ten million the fraction
  // goes; the window writes its own cards the same way ("46M", not "46.0M").
  if (n >= 1000000000LL) {
    snprintf(out, cap, "%.1fB", n / 1000000000.0);
  } else if (n >= 10000000LL) {
    snprintf(out, cap, "%.0fM", n / 1000000.0);
  } else if (n >= 1000000LL) {
    snprintf(out, cap, "%.1fM", n / 1000000.0);
  } else if (n >= 1000LL) {
    snprintf(out, cap, "%lldk", (n + 500) / 1000);
  } else {
    snprintf(out, cap, "%lld", n);
  }
}

// fit cuts a label until it measures no more than room pixels. A character count
// cannot do this: "workbuddy-ai" and "claude" are 11 and 6 characters and fit
// the same box at different sizes.
void fit(char* out, size_t cap, const ui::Face& f, const char* s, int room) {
  snprintf(out, cap, "%s", s ? s : "");
  if (room <= 0) {
    out[0] = 0;
    return;
  }
  if (ui::width(f, out) <= room) return;
  size_t n = strlen(out);
  while (n > 1) {
    do {
      n--;
    } while (n > 0 && (static_cast<uint8_t>(out[n]) & 0xC0) == 0x80);
    out[n] = 0;
    char probe[64];
    snprintf(probe, sizeof probe, "%s~", out);
    if (ui::width(f, probe) <= room) {
      snprintf(out, cap, "%s", probe);
      return;
    }
  }
  out[0] = 0;
}

// blend draws a 4-bit alpha mask in colour: the bird takes its agent's colour.
void blend(M5Canvas& g, const uint8_t* alpha, int w, int h, int x0, int y0, uint16_t colour) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const int i = y * w + x;
      const int a = (i & 1) ? (alpha[i >> 1] & 0x0F) : (alpha[i >> 1] >> 4);
      ui::blendPixel(g, x0 + x, y0 + y, colour, a);
    }
  }
}

// blendBank is blend with the sprite sheared row by row, which is how a bird
// tips into a turn. bank_ was computed on every frame and then thrown away; a
// few pixels of shear a row is what turns it into visible motion.
void blendBank(M5Canvas& g, const uint8_t* alpha, int w, int h, int x0, int y0,
               uint16_t colour, bool flip, float bank) {
  const float k = bank / 24.0f * 4.0f;  // up to 4px across the body
  for (int y = 0; y < h; y++) {
    const int dx = int(k * (y - h / 2.0f) / (h / 2.0f));
    for (int x = 0; x < w; x++) {
      const int i = y * w + (flip ? (w - 1 - x) : x);
      const int a = (i & 1) ? (alpha[i >> 1] & 0x0F) : (alpha[i >> 1] >> 4);
      if (!a) continue;
      ui::blendPixel(g, x0 + x + dx, y0 + y, colour, a);
    }
  }
}

// dot is a filled disc of coverage a: the trail the bird leaves is a row of
// them, each fainter than the one in front. Cheap where a faded sprite is not.
void dot(M5Canvas& g, int cx, int cy, int r, uint16_t colour, int a) {
  if (a <= 0 || r <= 0) return;
  const int r2 = r * r;
  for (int y = -r; y <= r; y++) {
    for (int x = -r; x <= r; x++) {
      if (x * x + y * y > r2) continue;
      ui::blendPixel(g, cx + x, cy + y, colour, a);
    }
  }
}

// ring is a circle's outline of coverage a: a ripple going out from a node.
void ring(M5Canvas& g, int cx, int cy, int r, uint16_t colour, int a) {
  if (a <= 0 || r <= 0) return;
  constexpr int kSteps = 48;
  for (int i = 0; i < kSteps; i++) {
    const float th = i * (2.0f * PI / kSteps);
    ui::blendPixel(g, cx + int(r * cosf(th) + 0.5f), cy + int(r * sinf(th) + 0.5f), colour, a);
  }
}

// wire lives further down, after Wire and onWire: it is the same bezier the
// flight follows, and there has to be one definition of that curve, not two.


// ---------------------------------------------------------------- the stage

constexpr int kPageH = 204;  // what a page may draw in
constexpr int kTabY = 205;   // where the tab strip starts on the panel
constexpr int kTabH = 240 - kTabY;  // its height: 35
// Three page cells and a control: 96 each leaves the last 32 for the theme.
constexpr int kTabW = 72;   // four page cells
constexpr int kThemeX = 288;

// A swipe: how far a finger must move to be a swipe rather than a tap, and how
// far on release to change the page rather than slide back.
constexpr int kSwipeStart = 10;
constexpr int kSwipeCommit = 64;
// How long the release takes to travel a whole page, in ms.
constexpr uint32_t kSlideMs = 240;

// The route stage, folded into a V: agent top left, magpie bottom centre,
// provider top right. The window keeps its three nodes on one line because a
// window has the width for it — 124px cards and 56px gaps. Here the width is
// fixed at 320 and a single row left 33px between cards, which is not a flight,
// it is a hop. Folding puts the length in the axis the page had spare: each leg
// becomes 68px instead of 33, with the same cards in the same places across.
constexpr int kStageTop = 116;    // the two outer cards
constexpr int kStageLow = 176;    // the hub, between and below them
constexpr int kAgentX = 56, kAgentY = kStageTop;
constexpr int kHubX = 160, kHubY = kStageLow;
constexpr int kSeatX = 264, kSeatY = kStageTop;

struct Node {
  int x, y;  // centre
  int w, h;
};
constexpr Node kAgent = {kAgentX, kAgentY, 88, 48};
constexpr Node kHub = {kHubX, kHubY, 54, 48};
constexpr Node kSeat = {kSeatX, kSeatY, 88, 48};

// card is the window's .rt-node: a card with a hairline border rather than a
// floating shape. The border is what makes three of them read as one stage,
// and it is the part of the desktop page's identity a dot could not carry.
void card(M5Canvas& g, const Node& n, uint16_t border, uint16_t bg, int r) {
  g.fillRoundRect(n.x - n.w / 2, n.y - n.h / 2, n.w, n.h, r, bg);
  g.drawRoundRect(n.x - n.w / 2, n.y - n.h / 2, n.w, n.h, r, border);
}

// A wire is one of the two curves on the stage, and which way along it a flight
// goes. Not a Leg: that name is the enum a flight's leg is named by.
struct Wire {
  float x0, y0, x1, y1;
  bool rev;
  // How far the path arcs above the straight line between its ends. This is the
  // difference between a flight and a slide: two nodes are 41px apart and the
  // bird is 34 wide, so on a straight line there was no room left for motion.
  float bow;
};

constexpr float kBow = 7.0f;  // about 5px of apex: a leg is 68px on the
                          // diagonal, not the window's 56

Wire wireOf(int which) {
  switch (which) {
    case 0:  // agent → hub
      return {float(kAgent.x + kAgent.w / 2), float(kAgent.y), float(kHub.x - kHub.w / 2),
              float(kHub.y), false, kBow};
    case 1:  // hub → seat
      return {float(kHub.x + kHub.w / 2), float(kHub.y), float(kSeat.x - kSeat.w / 2),
              float(kSeat.y), false, kBow};
    case 2:  // seat → hub, a failure going back
      return {float(kHub.x + kHub.w / 2), float(kHub.y), float(kSeat.x - kSeat.w / 2),
              float(kSeat.y), true, kBow};
    default:  // hub → agent, the answer coming home
      return {float(kAgent.x + kAgent.w / 2), float(kAgent.y), float(kHub.x - kHub.w / 2),
              float(kHub.y), true, kBow};
  }
}

void onWire(const Wire& l, float t, float* x, float* y) {
  const float x0 = l.rev ? l.x1 : l.x0, x1 = l.rev ? l.x0 : l.x1;
  const float y0 = l.rev ? l.y1 : l.y0, y1 = l.rev ? l.y0 : l.y1;
  const float mx = (x0 + x1) / 2;
  const float my = (y0 + y1) / 2 - l.bow;
  const float u = 1 - t;
  *x = u * u * u * x0 + 3 * u * u * t * mx + 3 * u * t * t * mx + t * t * t * x1;
  *y = u * u * u * y0 + 3 * u * u * t * my + 3 * u * t * t * my + t * t * t * y1;
}

// wireDash strokes the wire in dashes between t0 and t1. The window's wires are
// dashed and flow while a request is in flight (.rt-wires path.live), and that
// flow is what carries direction on a line only forty pixels long — a solid
// line here says "there is a path", a moving dash says "something is on it".
void wireDash(M5Canvas& g, const Wire& l, float t0, float t1, uint16_t colour,
              float on, float off, float phase) {
  if (t1 <= t0) return;
  constexpr int kSteps = 128;
  float px, py, x, y;
  onWire(l, t0, &px, &py);
  float acc = phase;
  for (int i = 1; i <= kSteps; i++) {
    const float t = t0 + (t1 - t0) * (float(i) / kSteps);
    onWire(l, t, &x, &y);
    acc += sqrtf((x - px) * (x - px) + (y - py) * (y - py));
    if (fmodf(acc, on + off) < on) g.drawLine(px, py, x, y, colour);
    px = x;
    py = y;
  }
}

// strokeTo strokes the wire only as far as t, in the direction of travel: what
// the bird has already covered is lit and the rest is not, so the line itself
// says how far along the request is. Same bezier as wire(), stopped early.
void strokeTo(M5Canvas& g, const Wire& l, float t, uint16_t colour) {
  constexpr int kSeg = 24;
  const int n = int(t * kSeg + 0.5f);
  if (n <= 0) return;
  float px, py, x, y;
  onWire(l, 0.0f, &px, &py);
  for (int i = 1; i <= n; i++) {
    onWire(l, float(i) / kSeg, &x, &y);
    g.drawLine(px, py, x, y, colour);
    px = x;
    py = y;
  }
}

// the page's timings, in ms. A leg is now one hop of a two-hop journey — the
// request goes agent → magpie → provider and the answer comes back the same
// way — so each is shorter than the single 1400ms leg it used to be.
constexpr uint32_t kFlyOut = 620;
constexpr uint32_t kFlyShort = 480;
constexpr uint32_t kFlyBack = 420;
constexpr uint32_t kFlyHome = 620;
constexpr uint32_t kFlyHomeErr = 600;
constexpr uint32_t kHeld = 650;
// Fast enough to read as wings rather than as a slideshow: at 420 the flight
// was three frames of flap and looked like a sticker sliding along a line.
constexpr uint32_t kFlapMs = 240;
// How long a landed leg's ripple takes, in ms.
constexpr uint32_t kRippleMs = 520;

// --------------------------------------------------------------- the waffle

// The window's own waffle, at what 320 pixels can show: 32 columns of 7px cells
// with a 2px gap is 286 wide, six rows is 52 high. The window draws 400 cells
// (50x8); the shape is the same, the resolution is not.
constexpr int kWaffleCols = 32, kWaffleRows = 6;
constexpr int kWaffleCells = kWaffleCols * kWaffleRows;

// cellsOf is context.js's cellsOf, scaled down: the used cells are spread over
// the parts by their share of the prompt, largest remainder, so a part of any
// size gets at least one cell and the cells add up to used.
void cellsOf(const Context& c, uint8_t* out) {
  for (int i = 0; i < kWaffleCells; i++) out[i] = kPartCount;  // free

  const int scale = c.window > 0 ? (c.window > c.tokens ? c.window : c.tokens)
                                 : (c.tokens > 0 ? c.tokens : 1);
  int used = static_cast<int>(lround(static_cast<double>(c.tokens) / scale * kWaffleCells));
  used = clampi(used, 0, kWaffleCells);
  if (used == 0) return;

  // a frame that names no parts is drawn as one block of "used", never as an
  // invented breakdown
  if (!c.hasParts) {
    for (int i = 0; i < used; i++) out[i] = kPartChat;
    return;
  }

  int sum = 0;
  for (int i = 0; i < kPartCount; i++) sum += c.parts[i];
  if (sum <= 0) {
    for (int i = 0; i < used; i++) out[i] = kPartChat;
    return;
  }

  int got[kPartCount] = {0};
  float want[kPartCount] = {0};
  int total = 0;
  for (int i = 0; i < kPartCount; i++) {
    if (c.parts[i] <= 0) continue;
    want[i] = static_cast<float>(c.parts[i]) / sum * used;
    got[i] = want[i] >= 1.0f ? static_cast<int>(want[i]) : 1;
    total += got[i];
  }
  while (total != used) {
    int pick = -1;
    float best = -1e9f;
    for (int i = 0; i < kPartCount; i++) {
      if (c.parts[i] <= 0) continue;
      if (total > used && got[i] <= 1) continue;
      const float score = total > used ? -(got[i] - want[i]) : (want[i] - floorf(want[i]));
      if (score > best) best = score, pick = i;
    }
    if (pick < 0) break;
    got[pick] += total > used ? -1 : 1;
    total += total > used ? -1 : 1;
  }

  int at = 0;
  for (int i = 0; i < kPartCount && at < kWaffleCells; i++) {
    for (int n = 0; n < got[i] && at < kWaffleCells; n++) out[at++] = static_cast<uint8_t>(i);
  }
}
// drawWaffle is the window's waffle: a cell for each share of the window, one
// colour a part, the cells the prompt never reached left in the track.
void drawWaffle(M5Canvas& g, const Context& c, int cell, int gap, int radius,
                int x0, int y0) {
  uint8_t cells[kWaffleCells];
  cellsOf(c, cells);
  const int stride = cell + gap;
  for (int i = 0; i < kWaffleCells; i++) {
    const int col = i % kWaffleCols, row = i / kWaffleCols;
    const uint8_t k = cells[i];
    g.fillRoundRect(x0 + col * stride, y0 + row * stride, cell, cell, radius,
                    k < kPartCount ? kPartColor[k] : kTrack);
  }
}



// easeOut is the release's curve: quick away, slow into place.
float easeOut(float t) {
  const float u = 1 - t;
  return 1 - u * u * u;
}

}  // namespace

bool Screen::begin() {
  // begin sets the display up. It does not start the board: M5.begin() is
  // called once, in setup(), and calling it again here re-initialises the whole
  // board — USB CDC among it, which drops the device off the bus mid-session.
  M5.Display.setRotation(1);
  M5.Display.setBrightness(128);
  M5.Display.fillScreen(kBg);  // and again on a theme change, from setTheme
  stage_.setPsram(true);
  stage_.setColorDepth(16);
  if (!stage_.createSprite(320, 240)) return false;
  // the page a swipe brings in: a second buffer, also in PSRAM. Without it a
  // swipe could only show one page, which is not a swipe.
  stage2_.setPsram(true);
  stage2_.setColorDepth(16);
  stage2_.createSprite(320, 240);  // a board without it still draws, unslid
  tabs_.setPsram(true);
  tabs_.setColorDepth(16);
  tabs_.createSprite(320, kTabH);
  painted_ = false;
  return true;
}

void Screen::showPage(Page p) {
  if (page_ == p) return;
  page_ = p;
  offset_ = 0;
  animating_ = false;
  swiping_ = false;
  painted_ = false;
}

// ------------------------------------------------------------------- touch

void Screen::touchDown(int x, int y) {
  touching_ = true;
  downX_ = x;
  downY_ = y;
  swiping_ = false;
  dir_ = 0;
  // a touch stops a release still running, where it is: the finger wins
  animating_ = false;
}

void Screen::touchMove(int x) {
  if (!touching_) return;
  const int dx = x - downX_;
  if (!swiping_) {
    // a drag is a swipe once it is far enough and more across than down: a
    // finger scrolling down a long page should not change pages
    if (abs(dx) < kSwipeStart) return;
    const int next = static_cast<int>(page_) + (dx < 0 ? 1 : -1);
    // the first and last page have nowhere to go: the page stays put rather
    // than sliding into a blank
    if (next < 0 || next >= static_cast<int>(Page::kCount)) return;
    swiping_ = true;
    dir_ = dx < 0 ? 1 : -1;
    incoming_ = static_cast<Page>(next);
  }
  // the page follows the finger, held to one page's width
  offset_ = clampf(static_cast<float>(dx), -320.0f, 320.0f);
  painted_ = false;
}

void Screen::touchUp(int x, int y) {
  if (!touching_) return;
  touching_ = false;

  if (swiping_) {
    const float dx = static_cast<float>(x - downX_);
    // far enough, or flicked hard: finish the move
    const bool commit = (dir_ > 0 ? dx <= -kSwipeCommit : dx >= kSwipeCommit);
    animating_ = true;
    animStart_ = millis();
    animFrom_ = offset_;
    // committing slides the incoming page fully on, then becomes it; a cancel
    // slides the current page back
    animTo_ = commit ? (dir_ > 0 ? -320.0f : 320.0f) : 0.0f;
    swiping_ = false;
    return;
  }

  // a tap: the tab bar jumps to a page, anywhere else goes on to the next
  const int dx = abs(x - downX_);
  const int dy = abs(y - downY_);
  if (dx > kSwipeStart || dy > kSwipeStart) return;  // a nudge, not a tap
  if (downY_ >= kTabY) {
    if (x >= kThemeX) {
      setTheme(!gPal.isDark);
      return;
    }
    showPage(static_cast<Page>(clampi(x / kTabW, 0, 3)));
    return;
  }
  showPage(static_cast<Page>((static_cast<int>(page_) + 1) % 3));
}

// -------------------------------------------------------------------- tick

void Screen::set(const Frame& f) {
  // a frame older than the one on the glass is dropped, as the protocol says
  if (drawn_.seq && f.seq <= drawn_.seq) return;
  f_ = f;
  offline_ = false;
  // a route this display hasn't played yet starts a flight: a new request, or
  // another try of the same one (the routing moved on)
  if (f.hasRoute && (f.route.id != playedRoute_ || f.route.try_ != playedTry_)) {
    playedRoute_ = f.route.id;
    playedTry_ = f.route.try_;
    step_ = Step::kRequest;
    leg_ = Leg::kToHub;
    legIdx_ = 0;
    legStart_ = millis();
    legMs_ = f.route.moved ? kFlyShort : kFlyOut;
    faceLeft_ = false;
    bank_ = 0;
  }
  painted_ = false;
}

void Screen::setup(const char* ap, const char* ip) {
  snprintf(setupAp_, sizeof setupAp_, "%s", ap ? ap : "");
  snprintf(setupIp_, sizeof setupIp_, "%s", ip ? ip : "");
  setup_ = true;
  painted_ = false;
}

void Screen::offline(bool down) {
  if (offline_ == down) return;
  offline_ = down;
  painted_ = false;
}

void Screen::tick(uint32_t now) {
  // the flight's clock. A request is two hops, not one: through magpie to the
  // provider, because that is the route the page is drawing. The bird used to
  // stop at the hub and turn back, so the node on the right was never reached
  // and the diagram never said what it means.
  if (step_ == Step::kRequest) {
    if (now - legStart_ >= legMs_) {
      rippleAt_ = now;
      rippleX_ = int(x_);
      rippleY_ = int(y_);
      if (++legIdx_ < 2) {
        leg_ = Leg::kToSeat;
        legStart_ = now;
        legMs_ = f_.route.moved ? kFlyShort : kFlyOut;
      } else {
        step_ = Step::kHeld;
        legStart_ = now;
      }
    }
  } else if (step_ == Step::kHeld) {
    if (now - legStart_ >= kHeld) {
      step_ = Step::kAnswer;
      legIdx_ = 0;
      leg_ = Leg::kBackHub;
      legStart_ = now;
      legMs_ = f_.route.fail ? kFlyBack : kFlyOut;
    }
  } else if (step_ == Step::kAnswer) {
    if (now - legStart_ >= legMs_) {
      rippleAt_ = now;
      rippleX_ = int(x_);
      rippleY_ = int(y_);
      if (++legIdx_ < 2) {
        leg_ = Leg::kToAgent;
        legStart_ = now;
        legMs_ = f_.route.fail ? kFlyHomeErr : kFlyHome;
      } else {
        step_ = Step::kIdle;
      }
    }
  }

  // the release's clock: it ends either on the next page or back where it was
  if (animating_) {
    const float t = clampf(float(now - animStart_) / kSlideMs, 0.0f, 1.0f);
    offset_ = animFrom_ + (animTo_ - animFrom_) * easeOut(t);
    if (t >= 1.0f) {
      animating_ = false;
      if (animTo_ != 0.0f) {
        // it landed: the incoming page is the page, and the offset is nothing
        page_ = incoming_;
        offset_ = 0;
      }
      animTo_ = 0;
    }
    painted_ = false;
  }

  // a frame that changed nothing and a flight that is still costs nothing.
  // The ring counts as still flying: it is drawn after step_ goes idle, so the
  // page has to keep redrawing until it has faded out.
  const bool flying = step_ != Step::kIdle || (rippleAt_ && now - rippleAt_ < kRippleMs);
  const bool moving = touching_ || animating_ || offset_ != 0.0f;
  if (painted_ && !flying && !moving && f_.seq == drawn_.seq && offline_ == drawnOffline_) return;

  drawSlide(now);
  drawn_ = f_;
  drawnOffline_ = offline_;
  painted_ = true;
}

// --------------------------------------------------------------- the slide

void Screen::drawSlide(uint32_t now) {
  // Being set up takes the whole panel: no tabs, because there are no pages to
  // go to yet, and the one thing on the glass is the network to join.
  if (setup_) {
    paintSetup(stage_);
    stage_.pushSprite(0, 0);
    return;
  }

  // the page on the glass, and — while it is moving — the one coming in
  stage_.fillScreen(kBg);
  paintPage(stage_, page_, now);
  if (offline_) paintOffline(stage_);

  if (offset_ == 0.0f) {
    stage_.pushSprite(0, 0);
  } else {
    // The incoming page sits on the far side of the current one, so the two
    // travel together as one strip: the finger drags the strip, and whichever
    // page is under it shows. pushSprite clips what falls off the panel.
    stage2_.fillScreen(kBg);
    paintPage(stage2_, incoming_, now);
    if (offline_) paintOffline(stage2_);
    const int other = offset_ < 0 ? 320 : -320;
    stage2_.pushSprite(int(offset_) + other, 0);
    stage_.pushSprite(int(offset_), 0);
  }

  // the strip is pushed last, over both pages: it is the one thing that does
  // not move, so the reader keeps their place while the pages go by
  tabs_.fillScreen(kBg);
  paintTabs(tabs_);
  tabs_.pushSprite(0, kTabY);
}

void Screen::paintSetup(M5Canvas& g) {
  // Two facts and a fallback. The sheet a phone opens by itself is the real
  // interface; this is what the person at the desk reads when it does not, so
  // the network's name and the address are set large enough to copy by eye.
  g.fillRect(0, 0, 320, 240, kBg);
  ui::drawTop(g, UI_26, "Set up", 160, 30, kFg, ui::kCenter);
  ui::drawTop(g, UI_14, "Join this Wi-Fi network", 160, 72, kFg3, ui::kCenter);
  ui::drawTop(g, UI_18, setupAp_, 160, 94, kFg, ui::kCenter);
  ui::drawTop(g, UI_14, "then open", 160, 130, kFg3, ui::kCenter);
  ui::drawTop(g, UI_18, setupIp_, 160, 152, kFg, ui::kCenter);
  ui::drawTop(g, UI_11, "2.4 GHz networks only", 160, 198, kFg3, ui::kCenter);
}

void Screen::paintOffline(M5Canvas& g) {
  // over the page, under the tabs: the tabs keep working, so a board that lost
  // magpie can still be looked at
  g.fillRect(0, 0, 320, kPageH, kBg);
  ui::draw(g, UI_26, "magpie offline", 160, 96, kDanger, ui::kCenter);
  ui::draw(g, UI_14, "no frames", 160, 126, kFg3, ui::kCenter);
}

void Screen::paintPage(M5Canvas& g, Page p, uint32_t now) {
  switch (p) {
    case Page::kContext: paintContext(g); break;
    case Page::kUsage: paintUsage(g); break;
    case Page::kLimits: paintLimits(g); break;
    default: paintRoute(g, now); break;
  }
}

// ------------------------------------------------------------------- tabs

void Screen::paintTabs(M5Canvas& g) {
  // a flat bar: a hairline above it, the open page's name bright with a short
  // rule under it, the others dim. No boxes — a box here would be as loud as
  // the page's own content.
  // drawn in the strip's own coordinates: y 0 is the panel's kTabY
  g.fillRect(0, 0, 320, kTabH, kBg);
  g.fillRect(0, 0, 320, 1, kHairline);
  static const char* const kNames[4] = {"Route", "Context", "Usage", "Limits"};
  for (int i = 0; i < 4; i++) {
    const bool on = static_cast<int>(page_) == i;
    const int cx = kTabW * i + kTabW / 2;
    ui::draw(g, UI_14, kNames[i], cx, 21, on ? kFg : kFg3, ui::kCenter);
    if (on) {
      const int tw = ui::width(UI_14, kNames[i]);
      g.fillRect(cx - tw / 2, 30, tw, 2, kFg);
    }
  }

  // The theme control, at the bar's right end where it costs the pages nothing.
  // The glyph is what a tap gets you, not where you are: a sun while the theme
  // is dark, a moon while it is light.
  const int cx = kThemeX + (320 - kThemeX) / 2, cy = 17;
  if (gPal.isDark) {
    g.fillCircle(cx, cy, 4, kFg3);
    for (int i = 0; i < 8; i++) {
      const float a = i * (PI / 4.0f);
      g.drawLine(cx + int(cosf(a) * 6.5f), cy + int(sinf(a) * 6.5f),
                 cx + int(cosf(a) * 9.5f), cy + int(sinf(a) * 9.5f), kFg3);
    }
  } else {
    g.fillCircle(cx - 1, cy, 6, kFg3);
    g.fillCircle(cx + 3, cy - 2, 5, kBg);
  }
}

void Screen::setBattery(int level, bool charging) {
  if (level == battery_ && charging == charging_) return;
  battery_ = level;
  charging_ = charging;
  painted_ = false;  // the level is drawn on the page
}

void Screen::setTheme(bool dark) {
  if (gPal.isDark == dark) return;
  gPal = dark ? kPaletteDark : kPaletteLight;
  dark_ = dark;
  themeDirty_ = true;
  painted_ = false;
}

// ------------------------------------------------------------------ route

void Screen::paintRoute(M5Canvas& g, uint32_t now) {
  const Route& r = f_.route;
  char buf[64];

  // the agent, at the top in its own colour: the one thing the eye finds first
  const uint16_t hue = f_.hasRoute ? hueOf(r.agent) : kFg3;
  ui::drawTop(g, UI_34, f_.hasRoute ? r.agent : "no request", 16, 0, hue);

  // The pack, at the top right: the one thing on this page that is about the
  // display rather than about magpie. It is drawn as a battery and not as a
  // number alone because the shape is read at a glance from across a desk,
  // and the colour is the warning: amber at 20%, red at 10%, mint while it is
  // on the charger.
  if (battery_ >= 0) {
    const uint16_t bc =
        charging_ ? kOk : (battery_ <= 10 ? kDanger : (battery_ <= 20 ? kWarn : kFg2));
    constexpr int kBX = 280, kBY = 6, kBW = 22, kBH = 11;
    g.drawRoundRect(kBX, kBY, kBW, kBH, 3, bc);
    g.fillRect(kBX + kBW, kBY + 3, 2, 5, bc);  // the nub
    int w = (kBW - 4) * battery_ / 100;
    if (battery_ > 0 && w < 1) w = 1;
    if (w > 0) g.fillRect(kBX + 2, kBY + 2, w, kBH - 4, bc);
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", battery_);
    ui::drawTop(g, UI_11, pct, kBX - 6, kBY - 2, kFg3, ui::kRight);
  }

  // And what answered, at 18 in full white. This used to be "provider/model"
  // at 14 in grey: a slash-joined pair, half of it a repeat of the other half,
  // in the colour of a caption. That is why the model was unreadable — the one
  // fact the page exists to show was set as fine print. It now gets the
  // brightest ink on the page and a line to itself.
  if (f_.hasRoute && r.model[0]) {
    g.fillRoundRect(16, 46, 3, 22, 1, hue);
    fit(buf, sizeof buf, UI_18, r.model, 278);
    ui::drawTop(g, UI_18, buf, 26, HERO_H, kFg);
  }
  // the group only. The provider is on its own card below, and a name set twice
  // on one page is a name the eye has to check twice.
  if (f_.hasRoute && r.group[0]) {
    char gline[48];
    snprintf(gline, sizeof gline, "group %s", r.group);
    fit(gline, sizeof gline, UI_11, gline, 278);
    ui::drawTop(g, UI_11, gline, 26, HERO_H + 30, kFg3);
  }

  // The stage, as the window draws it: cards with a hairline border, the hub lit
  // with a ring, and wires that stay dashed until the request has been along
  // them. That is routing.css's own grammar — .rt-node, .rt-hub, .rt-wires —
  // re-set for 320 pixels. A node lost its icon and kept its name-and-sub stack:
  // that stack is what makes it read as a card rather than as a blob, and it is
  // the part of the desktop stage worth carrying over at this size.
  const Wire w0 = wireOf(0), w1 = wireOf(1);
  const bool live = step_ != Step::kIdle;
  // the .rtflow dash offset: 8px of travel over .7s, the window's own speed
  const float phase = live ? float(now % 700) / 700.0f * 8.0f : 0.0f;

  // how far along each wire the request has been, which is .lit against .rest
  float lit0 = 0, lit1 = 0;
  if (f_.hasRoute) {
    if (r.done || step_ == Step::kHeld || step_ == Step::kAnswer) {
      lit0 = lit1 = 1.0f;
    } else if (step_ == Step::kRequest) {
      const float e = legMs_ ? clampf(float(now - legStart_) / legMs_, 0.0f, 1.0f) : 1.0f;
      if (legIdx_ == 0) {
        lit0 = e;
      } else {
        lit0 = 1.0f;
        lit1 = e;
      }
    }
  }
  if (lit0 > 0) strokeTo(g, w0, lit0, hue);
  wireDash(g, w0, lit0, 1.0f, live ? hue : kHairline, 3.0f, 5.0f, phase);
  if (lit1 > 0) strokeTo(g, w1, lit1, hue);
  wireDash(g, w1, lit1, 1.0f, live ? hue : kHairline, 3.0f, 5.0f, phase);

  // The flight is drawn before the nodes, not after: a bird leaving a card has
  // to come out from behind it, and drawn on top it was a sticker stuck to the
  // card, which is what made the motion read as a slide.
  paintFlight(g, now);

  // the agent: its colour in a dot before the name, as .rt-src b::before does
  card(g, kAgent, kHairline, kSurface, 10);
  dot(g, kAgent.x - kAgent.w / 2 + 11, kAgent.y - 11, 3, hue, 15);
  fit(buf, sizeof buf, UI_14, f_.hasRoute ? r.agent : "--", kAgent.w - 26);
  ui::drawTop(g, UI_14, buf, kAgent.x - kAgent.w / 2 + 20, kAgent.y - 21, kFg);
  ui::drawTop(g, UI_11, "agent", kAgent.x, kAgent.y + 3, kFg3, ui::kCenter);

  // the hub: the window lights it with a ring (.rt-hub's box-shadow) and sets
  // its name under the mark
  g.fillRoundRect(kHub.x - kHub.w / 2 - 4, kHub.y - kHub.h / 2 - 4, kHub.w + 8,
                  kHub.h + 8, 13, kBg);
  card(g, kHub, kFg, kSurface, 10);
  blend(g, bird_hub_alpha, bird_hub_w, bird_hub_h, kHub.x - bird_hub_w / 2,
        kHub.y - 22, kFg);
  ui::drawTop(g, UI_11, "magpie", kHub.x, kHub.y + 3, kFg3, ui::kCenter);

  // the provider: the answer's state is its border and its sub line
  card(g, kSeat, r.fail ? kDanger : kHairline, kSurface, 10);
  // 14 while it fits and 11 when it does not: the longest provider id that
  // arrives is "workbuddy-ai", and setting every name for that one case left
  // "kimi" small in a card with room to spare.
  //
  // The window stacks a node: mark, then name, then what it counts. So does
  // this: the mark goes above the name rather than beside it, because the card
  // is 88 across and "workbuddy-ai" needs 75 of its 76 pixels of text width.
  // Only the silhouette survives at this size, so the mark is drawn as ink in
  // the page's own colour; a provider with no mark draws no icon at all, and
  // its name keeps the full width and the larger face.
  const char* prov = f_.hasRoute ? r.provider : "--";
  const Icon* mark = f_.hasRoute ? iconFor(r.provider) : nullptr;
  int tailY = kSeat.y + 2;
  if (mark) {
    blend(g, mark->alpha, mark->w, mark->h, kSeat.x - mark->w / 2, kSeat.y - 22, kFg);
    fit(buf, sizeof buf, UI_11, prov, kSeat.w - 12);
    ui::drawTop(g, UI_11, buf, kSeat.x, kSeat.y - 6, kFg, ui::kCenter);
    tailY = kSeat.y + 8;
  } else {
    const bool wide = ui::width(UI_14, prov) <= kSeat.w - 14;
    fit(buf, sizeof buf, wide ? UI_14 : UI_11, prov, kSeat.w - 14);
    ui::drawTop(g, wide ? UI_14 : UI_11, buf, kSeat.x, kSeat.y - 21, kFg, ui::kCenter);
  }

  // the one number the stage cannot show: how long, or how many tries
  char tailBuf[24] = {0};
  if (f_.hasRoute && r.tries > 1) {
    snprintf(tailBuf, sizeof tailBuf, "try %d of %d", r.try_, r.tries);
  } else if (f_.hasRoute && r.done) {
    if (r.fail) snprintf(tailBuf, sizeof tailBuf, "%d failed", r.status);
    else snprintf(tailBuf, sizeof tailBuf, "%lld ms", static_cast<long long>(r.ms));
  }
  if (tailBuf[0]) {
    ui::drawTop(g, UI_11, tailBuf, kSeat.x, tailY, r.fail ? kDanger : kFg3, ui::kCenter);
  }
}

// ---------------------------------------------------------------- context

void Screen::paintContext(M5Canvas& g) {
  const Context& c = f_.context;
  char buf[40];

  const float pct = c.window > 0 ? clampf(float(c.tokens) / c.window, 0.0f, 1.0f) : 0.0f;
  const uint16_t tone = pct >= 0.9f ? kDanger : (pct >= 0.7f ? kWarn : kFg);

  // the fraction, large: it is what the page is about
  if (c.window > 0) {
    snprintf(buf, sizeof buf, "%d%%", static_cast<int>(pct * 100 + 0.5f));
  } else {
    snprintf(buf, sizeof buf, "--");
  }
  ui::drawTop(g, UI_34, buf, 16, 2, tone);

  // and the sentence under it, one line, quiet
  char tok[16] = {0}, win[24] = {0};
  if (c.tokens > 0) kfmt(tok, sizeof tok, c.tokens);
  if (c.window > 0) kfmt(win, sizeof win, c.window);
  if (tok[0] && win[0]) {
    snprintf(buf, sizeof buf, "%s of %s tokens", tok, win);
  } else if (tok[0]) {
    snprintf(buf, sizeof buf, "%s tokens", tok);
  } else if (win[0]) {
    snprintf(buf, sizeof buf, "of %s", win);
  } else {
    buf[0] = 0;
  }
  if (buf[0]) ui::drawTop(g, UI_14, buf, 16, HERO_H, kFg2);

  // The waffle on a padded base. The grid reads as a panel of the page rather
  // than as pixels painted on the ground. The base takes the page's own width,
  // 16 to 304, so it lines up with the text above and below it; a base that
  // hugged the grid instead sat seven pixels inside that margin and read as a
  // card floating over the page rather than as part of it.
  //
  // The cells come down from 7px to 6px to pay for the padding: a 7px grid is
  // 286 wide, and any padding at all would push the base past the margin. The
  // cost is that the padding is not square — 17 either side, 10 above and
  // below, because the legend sits at 138 and a taller base would meet it.
  constexpr int kCell = 6, kGap = 2, kStride = kCell + kGap;
  constexpr int kGridW = kStride * (kWaffleCols - 1) + kCell;                   // 254
  constexpr int kGridH = kWaffleRows * kCell + (kWaffleRows - 1) * kGap;        // 46
  constexpr int kPadY = 10;
  constexpr int kPanelW = 288;
  const int panelH = kGridH + kPadY * 2;
  const int panelX = 16, panelY = 100 - panelH / 2;
  g.fillRoundRect(panelX, panelY, kPanelW, panelH, 14, kSurface);
  g.drawRoundRect(panelX, panelY, kPanelW, panelH, 14, kHairline);
  drawWaffle(g, c, kCell, kGap, 2, panelX + (kPanelW - kGridW) / 2, panelY + kPadY);

  // the legend: each part, its colour, how much of the prompt it is. Two
  // columns of three, so six fit under the grid without a small face.
  const int legendY = 138;
  int sum = 0;
  for (int i = 0; i < kPartCount; i++) sum += c.parts[i];
  for (int i = 0; i < kPartCount; i++) {
    const int col = i % 2, row = i / 2;
    const int x = 16 + col * 152, y = legendY + row * 20;
    const bool has = c.hasParts && c.parts[i] > 0;
    g.fillRoundRect(x, y + 4, 8, 8, 2, has ? kPartColor[i] : kTrack);
    ui::draw(g, UI_11, kPartName[i], x + 13, y + 10, has ? kFg2 : kFg3);
    if (has && sum > 0) {
      kfmt(buf, sizeof buf, c.parts[i]);
      ui::draw(g, UI_11, buf, x + 140, y + 10, kFg3, ui::kRight);
    }
  }
}

// ------------------------------------------------------------------ usage

// sevColor is how much is left, coloured: red when it stops work this session,
// amber when it is on the way there, mint when it is fine. One function rather
// than the same nested pair of comparisons at every call site.
uint16_t sevColor(int left) {
  if (left <= 15) return kDanger;
  if (left <= 35) return kWarn;
  return kOk;
}

// meter fills pct percent as blocks from the left. Blocks, not a continuous
// bar: at 320 pixels the count reads at a glance — "3 of 24 left" — where a
// thin bar only reads as "short". Empty blocks stay drawn as a track, so the
// fraction is visible even when the fill is small.


// resetLabel is when the window opens again. It is the other half of "12%":
// whether that is for the next hour or the next two days decides what you do
// about it. The bridge carries it as seconds and the old page threw it away.
void resetLabel(int64_t at, char* out, size_t cap) {
  out[0] = 0;
  if (at <= 0) return;
  const long long d = at / 86400, h = (at % 86400) / 3600, m = (at % 3600) / 60;
  // a zero part is left off: "resets in 2d 0h" is longer than "resets in 2d"
  // and says the same thing
  if (d > 0) {
    if (h > 0) snprintf(out, cap, "resets in %lldd %lldh", d, h);
    else snprintf(out, cap, "resets in %lldd", d);
  } else if (h > 0) {
    if (m > 0) snprintf(out, cap, "resets in %lldh %lldm", h, m);
    else snprintf(out, cap, "resets in %lldh", h);
  } else {
    snprintf(out, cap, "resets in %lldm", m > 0 ? m : 1);
  }
}

void Screen::paintUsage(M5Canvas& g) {
  const Usage& u = f_.usage;
  char num[16];

  if (!f_.hasUsage) {
    ui::drawTop(g, UI_14, "no usage yet", 160, 90, kFg3, ui::kCenter);
    return;
  }

  // The window's four Usage cards, in one row. The board cannot fit the detail
  // lines side by side at 320 pixels, so each cell stacks: what it counts, the
  // number, then the split that makes the number mean something. The row is one
  // card with dividers rather than four boxes, which is how the window draws it
  // and it saves the gutters.
  constexpr int kCellW = 72, kRowY = 4, kRowH = 84;
  g.fillRoundRect(16, kRowY, 288, kRowH, 10, kSurface);
  g.drawRoundRect(16, kRowY, 288, kRowH, 10, kHairline);
  for (int i = 1; i < 4; i++) {
    g.fillRect(16 + i * kCellW, kRowY + 8, 1, kRowH - 16, kHairline);
  }

  // what each cell says, in the order the window's cards run
  const char* labels[4] = {"TOKENS", "CACHE", "THINK", "CALLS"};
  char numbers[4][16];
  char det1[4][20] = {{0}}, det2[4][20] = {{0}};
  {
    char a[16], b[16];
    kfmt(a, sizeof a, u.in);
    kfmt(b, sizeof b, u.out);
    kfmt(numbers[0], sizeof numbers[0], u.in + u.out);
    snprintf(det1[0], sizeof det1[0], "in %s", a);
    snprintf(det2[0], sizeof det2[0], "out %s", b);
  }
  kfmt(numbers[1], sizeof numbers[1], u.cacheRead);
  {
    // hit rate is the share of the prompt the vendor's cache served: reads over
    // reads plus fresh input, which is what the window's 95% is
    const int64_t seen = u.cacheRead + u.in;
    if (seen > 0) {
      snprintf(det1[1], sizeof det1[1], "%d%% hit",
               int(float(u.cacheRead) * 100.0f / float(seen) + 0.5f));
    }
  }
  kfmt(numbers[2], sizeof numbers[2], u.reasoning);
  if (u.reasoning > 0) snprintf(det1[2], sizeof det1[2], "of out");
  snprintf(numbers[3], sizeof numbers[3], "%d", u.requests);
  if (u.errors > 0) snprintf(det1[3], sizeof det1[3], "%d failed", u.errors);
  if (u.hasCost) snprintf(det2[3], sizeof det2[3], "$%.2f", u.cost);

  for (int i = 0; i < 4; i++) {
    const int cx = 16 + i * kCellW + kCellW / 2;
    ui::drawTop(g, UI_11, labels[i], cx, kRowY + 8, kFg3, ui::kCenter);
    // 26 while it fits and 18 when it cannot: "743M" is four glyphs at 26px and
    // a 72px cell does not hold it, and a truncated number ("74~") is worse
    // than a smaller one.
    const bool big = ui::width(UI_26, numbers[i]) <= kCellW - 8;
    fit(numbers[i], sizeof numbers[i], big ? UI_26 : UI_18, numbers[i], kCellW - 6);
    ui::drawTop(g, big ? UI_26 : UI_18, numbers[i], cx,
                big ? kRowY + 22 : kRowY + 30,
                i == 3 && u.errors > 0 ? kDanger : kFg, ui::kCenter);
    if (det1[i][0]) {
      fit(det1[i], sizeof det1[i], UI_11, det1[i], kCellW - 6);
      ui::drawTop(g, UI_11, det1[i], cx, kRowY + 58, kFg3, ui::kCenter);
    }
    if (det2[i][0]) {
      fit(det2[i], sizeof det2[i], UI_11, det2[i], kCellW - 6);
      ui::drawTop(g, UI_11, det2[i], cx, kRowY + 72, kFg3, ui::kCenter);
    }
  }

  // The week below, as the window's chart: a bar a day, the body what the day
  // cost in tokens and the cap what the vendor's cache served of it. The scale
  // sits at the chart's top right, where the window puts its own.
  constexpr int kBase = 178, kTop = 106;
  if (f_.nbars == 0) {
    ui::drawTop(g, UI_11, "no history", 304, kTop, kFg3, ui::kRight);
    return;
  }
  int64_t maxV = 0;
  for (int i = 0; i < f_.nbars; i++) {
    if (f_.bars[i].v > maxV) maxV = f_.bars[i].v;
  }
  if (maxV <= 0) maxV = 1;
  // the period on the left and the scale on the right: the window puts a
  // period picker over this chart, and the board's page is one period, so it
  // says which one instead of offering a choice it cannot take
  ui::drawTop(g, UI_11, "7 days", 16, kTop - 14, kFg3);
  kfmt(num, sizeof num, maxV);
  ui::drawTop(g, UI_11, num, 304, kTop - 14, kFg3, ui::kRight);

  constexpr int kGap = 5;
  const int bw = (288 - kGap * (f_.nbars - 1)) / f_.nbars;
  for (int i = 0; i < f_.nbars; i++) {
    const Bar& b = f_.bars[i];
    const int x = 16 + i * (bw + kGap);
    const int h = int(float(b.v) / float(maxV) * float(kBase - kTop));
    if (h > 0) {
      g.fillRect(x, kBase - h, bw, h, kChart);
      g.fillRect(x, kBase - h, bw, 4, kChartCap);  // the cached share's cap
    }
    if (b.label[0]) {
      ui::drawTop(g, UI_11, b.label, x + bw / 2, kBase + 4, kFg3, ui::kCenter);
    }
  }
  g.fillRect(16, kBase, 288, 1, kHairline);
}

// pill is the allowance's meter: one capsule of a track, the fill a capsule
// inside it. Blocks counted a fraction well on the Context page's waffle;
// here the fraction is the whole point, and a continuous bar reads it faster
// than counting.
void pill(M5Canvas& g, int x, int y, int w, int h, int pct, uint16_t col) {
  g.fillRoundRect(x, y, w, h, h / 2, kTrack);
  int fw = pct <= 0 ? 0 : (w * pct) / 100;
  if (fw > 0) {
    if (fw < h) fw = h;  // a capsule cannot be thinner than its own diameter
    g.fillRoundRect(x, y, fw, h, h / 2, col);
  }
}

void Screen::paintLimits(M5Canvas& g) {
  char buf[40];

  if (f_.cards == 0) {
    ui::drawTop(g, UI_14, "no allowances", 160, 96, kFg3, ui::kCenter);
    return;
  }

  // Three cards, one a window: who serves it, which of its windows this is,
  // what that window counts, and when it opens again. They used to be a hero
  // and two ranks under a summary of the whole ledger, but the summary is the
  // Usage page's now, and a window's own numbers — "161.7 / 200" — say more
  // than a bare percentage. The tightest still sits on top: the order and the
  // colour both say what is about to run out.
  for (int i = 0; i < f_.cards && i < kCards; i++) {
    const Quota& q = f_.quotas[i];
    const uint16_t col = sevColor(q.left);
    const int y = 10 + i * 65;
    const Node c = {160, y + 29, 288, 58};
    card(g, c, kHairline, kSurface, 14);

    // who serves it, then which window; the number at the right in the
    // severity's colour
    const char* who = q.sub[0] ? q.sub : q.name;
    const char* what = (q.sub[0] && q.name[0]) ? q.name : "";
    fit(buf, sizeof buf, UI_14, who, 170);
    ui::drawTop(g, UI_14, buf, 26, y + 6, kFg);
    const int whoW = ui::width(UI_14, buf);
    if (what[0]) {
      char wbuf[40];
      snprintf(wbuf, sizeof wbuf, "%s", what);
      fit(wbuf, sizeof wbuf, UI_11, wbuf, 288 - 24 - whoW - 56);
      ui::drawTop(g, UI_11, wbuf, 26 + whoW + 6, y + 10, kFg3);
    }
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", q.left);
    ui::drawTop(g, UI_18, pct, 294, y + 5, col, ui::kRight);

    // the pill, and under it what the window counts and when it resets
    pill(g, 26, y + 28, 268, 10, q.left, col);
    if (q.disp[0]) {
      fit(buf, sizeof buf, UI_11, q.disp, 130);
      ui::drawTop(g, UI_11, buf, 26, y + 43, kFg3);
    }
    char rl[40];
    resetLabel(q.at, rl, sizeof rl);
    if (rl[0]) ui::drawTop(g, UI_11, rl, 294, y + 43, kFg3, ui::kRight);
  }
}

// ----------------------------------------------------------------- flight

void Screen::poseLeg(Leg leg, float e) {
  const Wire l = wireOf(int(leg));
  float x, y;
  onWire(l, e, &x, &y);
  x_ = x;
  y_ = y;

  // the bird banks into the way it flies, and turns about to fly left
  float ax, ay, bx, by;
  onWire(l, clampf(e - 0.03f, 0.0f, 1.0f), &ax, &ay);
  onWire(l, clampf(e + 0.03f, 0.0f, 1.0f), &bx, &by);
  float vx = bx - ax, vy = by - ay;
  if (fabsf(vx) + fabsf(vy) < 0.01f) vx = l.rev ? -1 : 1;
  faceLeft_ = vx < 0;
  const float aim = clampf(atan2f(vy, fabsf(vx)) * 180.0f / PI, -24.0f, 24.0f);
  bank_ += (aim - bank_) * 0.12f;
}

void Screen::paintFlight(M5Canvas& g, uint32_t now) {
  const uint16_t hue = f_.hasRoute ? hueOf(f_.route.agent) : kFg;

  // The rings a landed leg leaves behind. Drawn before anything else so the
  // nodes and the bird sit on top of them, the way a ripple passes under what
  // made it. They outlive step_: the last leg ends as the flight goes idle and
  // the ring still has half its life to run.
  if (rippleAt_ && now - rippleAt_ < kRippleMs) {
    const uint32_t age = now - rippleAt_;
    for (int i = 0; i < 2; i++) {
      const uint32_t start = i * 110;
      if (age < start) continue;
      const float p = float(age - start) / float(kRippleMs - start);
      if (p > 1.0f) continue;
      ring(g, rippleX_, rippleY_, 5 + int(p * 18), hue, int(13 * (1 - p)));
    }
  }

  if (step_ == Step::kIdle) return;

  if (step_ == Step::kRequest || step_ == Step::kAnswer) {
    const float e = legMs_ ? clampf(float(now - legStart_) / legMs_, 0.0f, 1.0f) : 1.0f;
    poseLeg(leg_, e);
    const bool answering = step_ == Step::kAnswer;
    const Wire lw = wireOf(int(leg_));

    // the line lights up behind the bird and stays dark ahead of it
    strokeTo(g, lw, e, hue);

    if (!answering) {
      // The 24px sprite, not the 34px one: the cards leave 33px between them,
      // and the window's own proportion is a ~40px bird in a 56px gap.
      const int f = int((now / kFlapMs) * bird_flight_s_frames) % bird_flight_s_frames;
      const BirdFrame& bf = bird_flight_s[f];
      // The dot is at the tip of the beak, not the middle of the bird — that is
      // where the page puts it, and it is why the bird trails the wire.
      constexpr int kBeakFromEdge = 2;
      const int bx = faceLeft_ ? int(x_) - kBeakFromEdge : int(x_) - bf.w + kBeakFromEdge;
      const int by = int(y_) - bf.h / 2;

      // The trail starts behind the bird's TAIL, not behind its beak. Sampled
      // from the beak it was drawn underneath the sprite and was never once
      // visible. birdFrac is the share of the leg the body covers, so it stays
      // right when the sprite or the gap changes.
      const float chord = hypotf(lw.x1 - lw.x0, lw.y1 - lw.y0);
      const float birdFrac = float(bf.w) / (chord * 1.08f + 1.0f);
      for (int k = 0; k <= 10; k++) {
        const float tk = e - birdFrac - k * 0.035f;
        if (tk < 0) break;
        float tx, ty;
        onWire(lw, tk, &tx, &ty);
        const int r = k < 3 ? 2 : 1;
        dot(g, int(tx + 0.5f), int(ty + 0.5f), r, hue, 12 - k);
      }

      blendBank(g, bf.alpha, bf.w, bf.h, bx, by, hue, faceLeft_, bank_);
      dot(g, int(x_), int(y_), 2, kFg, 12);
    } else {
      // The answer is a ring, not a filled dot: .pkt.back in the window's CSS
      // fills with the card and strokes the agent's colour, so a request going
      // out and an answer coming home are different shapes at a glance.
      const uint16_t c = f_.route.fail ? kDanger : hue;
      dot(g, int(x_), int(y_), 8, c, 3);   // the drop-shadow's glow
      dot(g, int(x_), int(y_), 4, kSurface, 15);
      ring(g, int(x_), int(y_), 4, c, 15);
      ring(g, int(x_), int(y_), 5, c, 14);
    }
  } else if (step_ == Step::kHeld) {
    // the provider is working: a ring leaves the node the request was sent to
    const uint32_t held = now - legStart_;
    const float p = float(held % 700) / 700.0f;
    ring(g, kSeat.x, kSeat.y, 12 + int(p * 12), hue, int(12 * (1 - p)));
  }
}

}  // namespace magpie
