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
// A swipe draws both pages at once: the one leaving and the one arriving, each
// pushed at an offset, so the page follows the finger and the next one comes in
// from the side. That needs a second canvas — the board has the PSRAM for it —
// and the release finishes the move or puts it back.
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
#pragma once

#include <M5Unified.h>

#include "magpie.h"

namespace magpie {

// Page is one of the three the screen shows.
// Four pages: the window's Usage page is long enough that splitting it in two
// is the honest move on a 320px screen — what was spent, then what is left.
enum class Page : uint8_t {
  kRoute = 0,
  kContext = 1,
  kUsage = 2,
  kLimits = 3,
  kCount = 4,
};

class Screen {
 public:
  bool begin();
  // set takes a new frame, and starts a flight if its route is one this
  // display hasn't played yet.
  void set(const Frame& f);
  // tick advances the flight and the slide, and redraws what changed. now is
  // millis().
  void tick(uint32_t now);
  // offline draws "magpie offline" over the page, or takes it away.
  void offline(bool down);

  // setup puts the board's own access point on the glass: the name to join and
  // the address to open. It takes the whole panel, tabs included — the pages
  // have nothing to show until the board is on a network.
  void setup(const char* ap, const char* ip);

  // setupFailed says the network it was given did not take. Without it a board
  // with a mistyped password says "magpie offline" for ever, which looks like
  // magpie's fault and tells nobody what to do next.
  void setupFailed(const char* ssid);

  // setupDone takes either of those away, once the board is on a network.
  void setupDone();

  // The touch panel, as the three things a finger does.
  //
  // down starts a touch; move follows it, and a drag past the panel's own
  // threshold becomes a swipe that draws both pages; up finishes it — far
  // enough and the page changes, less and it slides back. A touch that never
  // moves is a tap, and a tap on the tab bar jumps to that page.
  void touchDown(int x, int y);
  void touchMove(int x);
  void touchUp(int x, int y);

  void showPage(Page p);
  Page page() const { return page_; }
  const Frame& frame() const { return f_; }

  // The theme. setTheme repaints on the next tick; the flag is how the caller
  // learns a tap changed it, so the choice can be written to flash. The screen
  // never touches storage itself: the renderer compiles this file too.
  // The pack, handed in from outside. The renderer has no PMIC and screen.cpp
  // must not reach for one, so the level is told to the page rather than read
  // by it. A level below 0 means there is no gauge to read, and nothing is
  // drawn rather than a shape filled in with a guess.
  void setBattery(int level, bool charging);

  void setTheme(bool dark);
  bool darkTheme() const { return dark_; }
  bool themeDirty() const { return themeDirty_; }
  void clearThemeDirty() { themeDirty_ = false; }

 private:
  // a flight's legs, and where it is
  enum class Leg : uint8_t { kToHub, kToSeat, kBackHub, kToAgent };
  enum class Step : uint8_t { kIdle, kRequest, kHeld, kAnswer };

  void paintPage(M5Canvas& g, Page p, uint32_t now);
  void paintTabs(M5Canvas& g);
  void paintRoute(M5Canvas& g, uint32_t now);
  void paintContext(M5Canvas& g);
  void paintUsage(M5Canvas& g);
  void paintLimits(M5Canvas& g);
  void paintFlight(M5Canvas& g, uint32_t now);
  void paintOffline(M5Canvas& g);
  void paintSetup(M5Canvas& g);

  // drawSlide pushes the two pages at their offsets, the one leaving and the
  // one arriving.
  void drawSlide(uint32_t now);

  // pose puts the dot (and the bird carrying it) at e of the way along a leg,
  // heading the way it flies: the preview's pose(), and the page's.
  void poseLeg(Leg leg, float e);

  M5Canvas stage_{&M5.Display};   // the drawing buffer: no flicker, one blit
  M5Canvas stage2_{&M5.Display};  // the page a swipe is bringing in
  // The tab bar has a canvas of its own, so it can be pushed over the pages
  // after they have slid: a bar that moved with them would show two of itself
  // passing each other mid-swipe.
  M5Canvas tabs_{&M5.Display};
  Frame f_{};
  Frame drawn_{};  // what is on the glass, so a frame that changed nothing costs nothing
  bool drawnOffline_ = false;

  Page page_ = Page::kRoute;

  // the swipe
  bool touching_ = false;
  int downX_ = 0, downY_ = 0;
  bool swiping_ = false;  // the finger has passed the threshold
  int dir_ = 0;           // -1 next page, +1 previous, 0 not decided
  Page incoming_ = Page::kRoute;
  float offset_ = 0;  // how far the current page has moved, in pixels
  // the release: an animation from where the finger left it to 0 or a page
  bool animating_ = false;
  uint32_t animStart_ = 0;
  float animFrom_ = 0, animTo_ = 0;

  // the flight
  Step step_ = Step::kIdle;
  Leg leg_ = Leg::kToHub;
  uint32_t legStart_ = 0, legMs_ = 0;
  float x_ = 0, y_ = 0;  // where the dot is
  float bank_ = 0;       // the bird's tilt, -24..24 degrees
  bool faceLeft_ = false;
  // the rings a landed leg leaves behind: where, and when. Kept apart from
  // step_ because they outlive it — the last leg ends as the flight goes idle,
  // and the ring has to keep drawing after that.
  uint32_t rippleAt_ = 0;
  int rippleX_ = 0, rippleY_ = 0;
  // which hop of the current step this is: 0 the first, 1 the second. A request
  // is agent → magpie → provider, an answer the same way back.
  int legIdx_ = 0;
  int64_t playedRoute_ = 0;  // the route id last started, so a redraw isn't a replay
  int playedTry_ = 0;

  bool offline_ = false;
  enum class Setup : uint8_t { kOff, kWaiting, kFailed };
  Setup setup_ = Setup::kOff;
  char setupAp_[32] = {0};
  char setupIp_[16] = {0};
  char setupSsid_[33] = {0};
  bool painted_ = false;
  int battery_ = -1;         // 0..100, -1 unknown
  bool charging_ = false;
  bool dark_ = true;         // the theme on the glass
  bool themeDirty_ = false;  // a tap changed it, and the caller should save it
};

}  // namespace magpie
