// magpie's frame, as docs/protocol.md defines it, and the limits the display
// keeps to. Everything here is fixed-size: the board has 512 KiB of SRAM and
// the frame is never bigger than 2048 bytes, so nothing allocates per frame.
#pragma once

#include <stdint.h>

namespace magpie {

// The wire version this firmware speaks. A frame with a higher one is drawn
// as far as it is understood, and the screen says so.
constexpr int kFrameVersion = 1;

// The longest body magpie sends. Anything longer is refused with 413.
constexpr int kMaxBody = 2048;

// The longest a name may be, and how many quota cards are drawn.
constexpr int kNameMax = 24;
constexpr int kCardMax = 16;
constexpr int kCards = 3;

// A route: the newest request, and where routing put it.
struct Route {
  int64_t id = 0;
  char agent[kNameMax + 1] = {0};
  char provider[kNameMax + 1] = {0};
  char model[kNameMax + 1] = {0};
  char group[kNameMax + 1] = {0};
  int try_ = 0;    // which try this is, 1-based
  int tries = 0;   // how many the routing offered
  bool moved = false;  // a try before this failed
  bool done = false;
  int status = 0;
  int64_t ms = 0;
  int64_t ttft = 0;
  bool fail = false;

  bool present() const { return id != 0; }
};

// The parts a prompt is made of, in magpie's own order (PARTS in
// internal/gui/assets/context.js). The board draws them as the window's own
// waffle: one colour a part, the free cells left empty.
enum PartKind : uint8_t {
  kPartSystem = 0,
  kPartTools,
  kPartMemory,
  kPartFiles,
  kPartResults,
  kPartChat,
  kPartCount,
};

// The prompt, and the window it was measured against.
struct Context {
  int tokens = 0;
  int window = 0;  // 0 when the model's window isn't known
  int cache = 0;
  int out = 0;
  // parts are the tokens of each PartKind. A frame that names none leaves
  // them all 0, and the board then draws the used/free split without the
  // colours — it never invents a breakdown.
  int parts[kPartCount] = {0};
  bool hasParts = false;
};

// Today, from the usage ledger.
// Usage is the window's Usage cards, as numbers: what a period cost in tokens
// and what those tokens were. tokens used to be the only one carried, the four
// kinds already summed, which is all a "2.4M tokens" line can say.
struct Usage {
  int requests = 0;
  int64_t tokens = 0;      // in + out + cache read + cache write
  int errors = 0;
  int64_t in = 0;          // the prompt's own tokens
  int64_t out = 0;         // the reply's
  int64_t cacheRead = 0;   // served from the vendor's cache
  int64_t reasoning = 0;   // billed as output, counted apart
  bool hasCost = false;
  float cost = 0;
};

// One bar of the week's chart: a day's tokens, and how much of it came out of
// the cache. The window stacks the two, a pale body and a darker cap.
struct Bar {
  char label[4] = {0};  // the day of the month, "15"
  int64_t v = 0;
  int64_t c = 0;
};
constexpr int kBars = 7;

// One window of one card.
struct Quota {
  char name[kCardMax + 1] = {0};   // the window: "Chat requests"
  char sub[kCardMax + 1] = {0};    // who serves it: "Copilot". Two
                                   // subscriptions both name a window
                                   // "Credits", and a card that says only
                                   // that says nothing about which pool.
  char disp[kCardMax + 1] = {0};   // what it counts: "161.7 / 200"
  int left = 0;
  int64_t at = 0;  // seconds until it resets, 0 for none
};

// One whole snapshot: never a delta.
struct Frame {
  int v = 0;
  int64_t seq = 0;
  int64_t now = 0;
  bool up = false;

  bool hasRoute = false;
  Route route;

  bool hasContext = false;
  Context context;

  bool hasUsage = false;
  Usage usage;

  // the week's chart: oldest first, up to seven days
  int nbars = 0;
  Bar bars[kBars];

  int cards = 0;
  Quota quotas[kCards];

  // newer is whether f is a frame this display should take: a monotonic seq,
  // so a reordered or repeated datagram is dropped rather than drawn.
  bool newer(const Frame& f) const { return f.seq > seq; }
};

// parse reads one body into f. It returns false with a reason when the body
// isn't a frame at all; a frame it can read partly is read partly, which is
// what a newer magpie adding a key looks like.
bool parse(const char* body, Frame& f, const char** why);

}  // namespace magpie
