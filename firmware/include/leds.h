// The strip: twelve WS2812Cs on StackChan's body, and what they say about
// routing.
//
// They are not on an ESP32 GPIO. StackChan's schematic drives their data line
// from the PY32L020 IO expander (M5IOE1) at I2C 0x6F, IO14, so they are written
// as registers and not bit-banged: Adafruit_NeoPixel, FastLED and the RMT
// peripheral cannot reach them, and a strip driver pointed at a GPIO would
// drive nothing at all. The expander holds a 32-entry buffer of RGB565 values
// (0x30 + index*2) and shifts it out to the chain when the REFRESH bit (0x24
// bit 6) is written.
//
// A bare CoreS3 has no expander at 0x6F and this is then a no-op: begin() reads
// the version register and finds 0 or 0xFF.
//
// The colours are the panel's: the agent's own hue while a request is out, the
// screen's mint when an answer lands and its red when a try fails. The light is
// the same fact the flight draws, off the glass, so a desk can be read from
// across the room.
#pragma once

#include <stdint.h>

namespace magpie {

// Led is what the strip is saying.
enum class Led : uint8_t {
  kIdle,     // nothing new: a slow, dim breath
  kBusy,     // a request is out: the agent's hue, breathing faster
  kOk,       // an answer landed
  kFail,     // the newest try failed
  kOffline,  // no frame for 45 s: a barely-there amber
  kSetup,    // the access point is up, waiting for a phone
};

class Leds {
 public:
  // begin finds the expander and takes the strip to black. False means there is
  // no strip on this board, which is every bare CoreS3.
  bool begin();
  bool present() const { return ok_; }

  // set is what to say, and the hue to say kBusy in (RGB565). A state change
  // restarts the breath, so the light always starts from dark and rises.
  void set(Led s, uint16_t hue565);

  // tick advances the breath and writes it, at its own slower rate: a breath
  // over seconds does not need the panel's frame rate, and the bus is shared
  // with the touch panel and the PMIC.
  void tick(uint32_t now);

  const char* stateName() const;

 private:
  void write(uint16_t c565, int level);
  void blank();
  // periodOf is how long one breath of the current state lasts; levelOf is what
  // to show at `now` (0..255), and colorOf is the colour it shows at all.
  uint32_t periodOf() const;
  int levelOf(uint32_t now) const;
  uint16_t colorOf() const;

  bool ok_ = false;
  Led state_ = Led::kIdle;
  uint32_t stateAt_ = 0;
  uint32_t lastWrite_ = 0;
  uint16_t hue_ = 0;
  // What is on the chain, so a breath that rounded to the same values costs no
  // bus time. A 24-byte burst every 40 ms is cheap; this makes it nearly free.
  uint16_t shown_[12] = {0};
  bool blanked_ = false;
};

extern Leds leds;

}  // namespace magpie
