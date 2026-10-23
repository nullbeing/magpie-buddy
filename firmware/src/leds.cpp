// See leds.h: the twelve WS2812Cs, reached through StackChan's IO expander.
#include "leds.h"

#include <M5Unified.h>
#include <math.h>

namespace magpie {
namespace {

// The PY32L020 (M5IOE1) that drives the chain's data line. Not a NeoPixel GPIO:
// see the header.
constexpr uint8_t kAddr = 0x6F;
// 0x02 is the firmware revision, which reads 0 or 0xFF when nothing answers.
constexpr uint8_t kRegVersion = 0x02;
// LED_CFG: the low six bits are how many pixels to shift out, bit 6 latches the
// buffer into the chain. A count of 0 disables the driver.
constexpr uint8_t kRegLedCfg = 0x24;
// The pixel buffer: one RGB565 per light, low byte first, up to 32 of them. The
// twelve here are a burst of 24 bytes at 400 kHz, which is under a millisecond.
constexpr uint8_t kRegLedRam = 0x30;
// Direction and drive-mode registers, high byte: pins 8..15, so pin 13 is bit 5.
constexpr uint8_t kRegGpioMHi = 0x04;
constexpr uint8_t kRegGpioDrvHi = 0x14;
// Pin 13 of the expander is IO14, the net the chain's DIN is on. The expander
// hands the pin to its NeoPixel hardware, but the BSP drives it push-pull first
// and so does this: the same sequence, so a strip that works there works here.
constexpr uint8_t kPinRgb = 13;
constexpr uint32_t kBusFreq = 400000;

constexpr int kCount = 12;
// 25 frames a second. A breath measured in seconds does not need more, and the
// bus is shared with the PMIC and the touch panel.
constexpr uint32_t kStepMs = 40;

// The panel's own mint, red and amber, so the strip says what the glass says.
constexpr uint16_t kMint = 0x3EF2;
constexpr uint16_t kRed = 0xE28A;
constexpr uint16_t kAmber = 0xDD07;

uint16_t scale(uint16_t c, int level) {
  const int r = ((c >> 11) & 0x1F) * level / 255;
  const int g = ((c >> 5) & 0x3F) * level / 255;
  const int b = (c & 0x1F) * level / 255;
  // RGB565, so sixteen bits: returning this as uint8_t drops the red field
  // entirely and every colour comes out blue-green.
  return uint16_t((r << 11) | (g << 5) | b);
}

// A breath: 0 at both ends, 255 in the middle, with flat ends so the light
// never snaps on. A raised cosine rather than a triangle for the same reason.
int breath(uint32_t ms, uint32_t period) {
  const float p = float(ms % period) / float(period);
  return int((0.5f - 0.5f * cosf(p * 6.2831853f)) * 255.0f + 0.5f);
}

}  // namespace

Leds leds;

bool Leds::begin() {
  // The expander can be slow to boot; a board that answers on the second look
  // should not be written off as a bare CoreS3.
  uint8_t version = 0;
  for (int i = 0; i < 4; i++) {
    version = m5::In_I2C.readRegister8(kAddr, kRegVersion, kBusFreq);
    if (version != 0 && version != 0xFF) break;
    delay(100);
  }
  if (version == 0 || version == 0xFF) return false;

  ok_ = true;
  // Read-modify-write, never a blind store. Pin 0 of this expander is VM_EN,
  // which is the servo rail's power switch: writing GPIO_M_L = 0 would turn pin
  // 0 back into an input, drop the rail, and let StackChan's head go limp. The
  // strip only owns pin 13, so only pin 13's bits are touched.
  const uint8_t mHi = m5::In_I2C.readRegister8(kAddr, kRegGpioMHi, kBusFreq);
  m5::In_I2C.writeRegister8(kAddr, kRegGpioMHi, uint8_t(mHi | (1 << (kPinRgb - 8))), kBusFreq);
  const uint8_t drvHi = m5::In_I2C.readRegister8(kAddr, kRegGpioDrvHi, kBusFreq);
  // push-pull, not the expander's open-drain default: the chain needs a driven
  // high, and there is no pull-up on the net to do it instead.
  m5::In_I2C.writeRegister8(kAddr, kRegGpioDrvHi, uint8_t(drvHi & ~(1 << (kPinRgb - 8))), kBusFreq);
  // Twelve lights, no refresh yet: the first latch is blank(), below.
  m5::In_I2C.writeRegister8(kAddr, kRegLedCfg, kCount, kBusFreq);
  blank();
  return true;
}

// blank puts the chain out. Twice, as the BSP does: the first burst is what
// wakes a chain that was left mid-frame.
void Leds::blank() {
  if (!ok_) return;
  uint8_t buf[kCount * 2] = {0};
  for (int i = 0; i < 2; i++) {
    m5::In_I2C.writeRegister(kAddr, kRegLedRam, buf, sizeof buf, kBusFreq);
    m5::In_I2C.writeRegister8(kAddr, kRegLedCfg, uint8_t(kCount | 0x40), kBusFreq);
    delay(20);
  }
  for (int i = 0; i < kCount; i++) shown_[i] = 0;
  blanked_ = true;
}

// write puts one colour on all twelve, at a brightness of 0..255.
void Leds::write(uint16_t c565, int level) {
  if (!ok_) return;
  const uint16_t c = scale(c565, level);
  bool same = true;
  for (int i = 0; i < kCount; i++) {
    if (shown_[i] != c) {
      same = false;
      break;
    }
  }
  if (same && !blanked_) return;

  uint8_t buf[kCount * 2];
  for (int i = 0; i < kCount; i++) {
    buf[i * 2] = uint8_t(c & 0xFF);
    buf[i * 2 + 1] = uint8_t(c >> 8);
    shown_[i] = c;
  }
  m5::In_I2C.writeRegister(kAddr, kRegLedRam, buf, sizeof buf, kBusFreq);
  // The count is written back with the refresh bit: without it the buffer is
  // only memory, and the chain shows whatever it was last latched with.
  m5::In_I2C.writeRegister8(kAddr, kRegLedCfg, uint8_t(kCount | 0x40), kBusFreq);
  blanked_ = false;
}

void Leds::set(Led s, uint16_t hue565) {
  // Idle, offline and setup have a colour each of their own; the hue is only
  // kBusy's to use. It is taken out before the comparison, not after: a caller
  // passing an agent hue on every frame would otherwise never match what is
  // stored, and the breath would restart at the loop's rate instead of the
  // state's.
  const uint16_t h = (s == Led::kIdle || s == Led::kOffline || s == Led::kSetup) ? 0 : hue565;
  // Only a change restarts the breath. loop() calls this every pass, and a
  // breath restarted 500 times a second would look like a flicker.
  if (s == state_ && h == hue_) return;
  state_ = s;
  hue_ = h;
  stateAt_ = millis();
  lastWrite_ = 0;  // write on the next tick, not up to kStepMs later
}

uint32_t Leds::periodOf() const {
  switch (state_) {
    case Led::kBusy:
      return 1500;
    case Led::kOffline:
      return 6000;
    case Led::kSetup:
      return 2000;
    case Led::kIdle:
    default:
      return 4000;
  }
}

// levelOf is what to show at `now`. The one-shot states — an answer landing, a
// try failing — decay from full to dark and then stay dark, so a desk sees one
// event once instead of a light that keeps saying it.
int Leds::levelOf(uint32_t now) const {
  const uint32_t t = now - stateAt_;
  switch (state_) {
    case Led::kOk: {
      constexpr uint32_t kFade = 2400;
      return t >= kFade ? 0 : 255 - int(255 * t / kFade);
    }
    case Led::kFail: {
      // Two quick blinks, then dark: a failure is not a slow breath.
      constexpr uint32_t kFade = 2600;
      if (t >= kFade) return 0;
      const int peak = 255 - int(255 * t / kFade);
      const int phase = int(t % 400);
      return phase < 200 ? peak : 0;
    }
    case Led::kBusy:
      return breath(t, periodOf());
    case Led::kSetup:
      return 30 + breath(t, periodOf()) * 150 / 255;
    case Led::kOffline:
      return 2 + breath(t, periodOf()) * 32 / 255;
    case Led::kIdle:
    default:
      return 6 + breath(t, periodOf()) * 74 / 255;
  }
}

uint16_t Leds::colorOf() const {
  switch (state_) {
    case Led::kOk:
      return kMint;
    case Led::kFail:
      return kRed;
    case Led::kOffline:
      return kAmber;
    case Led::kSetup:
      return kMint;
    case Led::kBusy:
      // the agent's own hue, and a neutral if a frame named none
      return hue_ ? hue_ : 0xFFFF;
    case Led::kIdle:
    default:
      return 0xFFFF;
  }
}

void Leds::tick(uint32_t now) {
  if (!ok_) return;
  // A landing and a failure are flashes, not states: once the decay reaches
  // black the strip rests at the idle breath, which is what a desk light that
  // is on and listening looks like. Done here rather than in the caller, so a
  // loop that repeats the same frame does not re-flash it.
  if ((state_ == Led::kOk || state_ == Led::kFail) && levelOf(now) == 0) {
    state_ = Led::kIdle;
    hue_ = 0;
    stateAt_ = now;
    lastWrite_ = 0;
  }
  if (lastWrite_ && now - lastWrite_ < kStepMs) return;
  lastWrite_ = now;
  write(colorOf(), levelOf(now));
}

const char* Leds::stateName() const {
  switch (state_) {
    case Led::kBusy:
      return "busy";
    case Led::kOk:
      return "ok";
    case Led::kFail:
      return "fail";
    case Led::kOffline:
      return "offline";
    case Led::kSetup:
      return "setup";
    case Led::kIdle:
    default:
      return "idle";
  }
}

}  // namespace magpie
