// A host shim for M5GFX/M5Unified.
//
// The firmware's screen is drawn by screen.cpp. To look at it without a board —
// which matters when the board is not on USB, or when a layout change has to be
// judged before it is flashed — this shim stands in for the graphics library and
// turns each drawing call into SVG. The firmware's code is compiled unchanged,
// so what comes out is the layout the board would draw.
//
// A sprite is a buffer of its own, as it is on the board: drawing into one
// leaves the others alone, and only pushSprite/pushImage puts it on the panel.
// That matters here — the tab strip is drawn into its own sprite after the pages
// have been pushed, and a shim with one shared output would have it erase them.
//
// There are no fonts here on purpose. The board draws its text from
// ui_text.h's baked Inter atlases, which are compiled into both the firmware and
// this renderer — so the preview shows the board's own text.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef PI
#define PI 3.14159265358979323846f
#endif

// millis() is the board's clock. A test sets it to pose a flight.
extern uint32_t g_millis;
inline uint32_t millis() { return g_millis; }

namespace svg {
// out is the panel: what has been pushed to it, in the order it was pushed.
extern std::string out;
}  // namespace svg

namespace shim {

inline void rgb565(uint16_t c, int& r, int& g, int& b) {
  r = ((c >> 11) & 0x1F) * 255 / 31;
  g = ((c >> 5) & 0x3F) * 255 / 63;
  b = (c & 0x1F) * 255 / 31;
}

inline std::string hex(uint16_t c) {
  int r, g, b;
  rgb565(c, r, g, b);
  char buf[8];
  snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
  return buf;
}

inline std::string num(float v) {
  char buf[24];
  snprintf(buf, sizeof buf, "%.1f", v);
  return buf;
}

}  // namespace shim

// The panel is 320x240 whatever the sprite is.
constexpr int kPanelW = 320, kPanelH = 240;

class M5Canvas {
 public:
  // A sprite owns its pixels from the moment it exists: the board's
  // M5.Display is a panel sprite that was never created with createSprite, and
  // the firmware draws into it directly.
  explicit M5Canvas(M5Canvas* = nullptr) { alloc(kPanelW, kPanelH); }
  ~M5Canvas() { delete[] px_; }
  // A sprite is a buffer: copying one would share it and free it twice.
  M5Canvas(const M5Canvas&) = delete;
  M5Canvas& operator=(const M5Canvas&) = delete;

  int width() const { return w_; }
  int height() const { return h_; }

  uint16_t color565(int r, int g, int b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }

  bool createSprite(int w, int h) {
    alloc(w, h);
    return true;
  }

  void setPsram(bool) {}
  void setColorDepth(int) {}
  void setTextWrap(bool) {}
  void setRotation(int) {}
  void setBrightness(int) {}

  // The panel's own blit: a sprite pushed at a position. Off-panel pixels are
  // clipped, as pushImage clips them.
  void pushSprite(int x, int y) {
    svg::out += "<g transform=\"translate(" + std::to_string(x) + " " + std::to_string(y) + ")\">\n";
    svg::out += buf_;
    svg::out += "</g>\n";
  }

  void pushImage(int x, int y, int w, int h, M5Canvas& src) {
    // the same composite, cut to the rectangle asked for
    svg::out += "<g transform=\"translate(" + std::to_string(x) + " " + std::to_string(y) + ")\">\n";
    svg::out += "<clipPath id=\"c" + std::to_string(++clip_) + "\"><rect x=\"0\" y=\"0\" width=\"" +
                std::to_string(w) + "\" height=\"" + std::to_string(h) + "\"/></clipPath>\n";
    svg::out += "<g clip-path=\"url(#c" + std::to_string(clip_) + ")\">\n" + src.buf_ + "</g>\n</g>\n";
  }

  void fillScreen(uint16_t c) {
    bg_ = c;
    buf_.clear();
    for (int i = 0; i < w_ * h_; i++) px_[i] = c;
    buf_ += "<rect x=\"0\" y=\"0\" width=\"" + std::to_string(w_) + "\" height=\"" +
            std::to_string(h_) + "\" fill=\"" + shim::hex(c) + "\"/>\n";
  }

  void fillRect(int x, int y, int w, int h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    fillPixels(x, y, w, h, c);
    buf_ += "<rect x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) +
            "\" width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) +
            "\" fill=\"" + shim::hex(c) + "\"/>\n";
  }

  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    fillPixels(x, y, w, h, c);
    buf_ += "<rect x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) +
            "\" width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) +
            "\" rx=\"" + std::to_string(r) + "\" fill=\"" + shim::hex(c) + "\"/>\n";
  }

  void drawRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    buf_ += "<rect x=\"" + shim::num(x + 0.5f) + "\" y=\"" + shim::num(y + 0.5f) +
            "\" width=\"" + shim::num(w - 1.0f) + "\" height=\"" + shim::num(h - 1.0f) +
            "\" rx=\"" + std::to_string(r) + "\" fill=\"none\" stroke=\"" + shim::hex(c) +
            "\" stroke-width=\"1\"/>\n";
  }

  void fillCircle(int x, int y, int r, uint16_t c) {
    if (r <= 0) return;
    buf_ += "<circle cx=\"" + std::to_string(x) + "\" cy=\"" + std::to_string(y) +
            "\" r=\"" + std::to_string(r) + "\" fill=\"" + shim::hex(c) + "\"/>\n";
  }

  void drawCircle(int x, int y, int r, uint16_t c) {
    buf_ += "<circle cx=\"" + std::to_string(x) + "\" cy=\"" + std::to_string(y) +
            "\" r=\"" + std::to_string(r) + "\" fill=\"none\" stroke=\"" + shim::hex(c) +
            "\" stroke-width=\"1\"/>\n";
  }

  void drawLine(int x0, int y0, int x1, int y1, uint16_t c) {
    buf_ += "<line x1=\"" + std::to_string(x0) + "\" y1=\"" + std::to_string(y0) +
            "\" x2=\"" + std::to_string(x1) + "\" y2=\"" + std::to_string(y1) +
            "\" stroke=\"" + shim::hex(c) + "\" stroke-width=\"1\"/>\n";
  }

  void drawPixel(int x, int y, uint16_t c) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    px_[y * w_ + x] = c;
    buf_ += "<rect x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) +
            "\" width=\"1\" height=\"1\" fill=\"" + shim::hex(c) + "\"/>\n";
  }

  // readPixel is what a blend reads before it blends: the text's coverage and
  // the bird's alpha both do. It reads this sprite's own pixels, as on the
  // board — a sprite has no idea what is under it.
  uint16_t readPixel(int x, int y) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return bg_;
    return px_[y * w_ + x];
  }

 private:
  void alloc(int w, int h) {
    w_ = w;
    h_ = h;
    delete[] px_;
    px_ = new uint16_t[static_cast<size_t>(w) * h]();
    buf_.clear();
  }

  // fillPixels keeps the pixel buffer in step with the SVG for the shapes a
  // blend can read back. A rect is the only one that matters in practice.
  void fillPixels(int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++) {
      if (yy < 0 || yy >= h_) continue;
      for (int xx = x; xx < x + w; xx++) {
        if (xx < 0 || xx >= w_) continue;
        px_[yy * w_ + xx] = c;
      }
    }
  }

  int w_ = kPanelW, h_ = kPanelH;
  uint16_t* px_ = nullptr;
  uint16_t bg_ = 0;
  std::string buf_;  // this sprite's own drawing, not the panel's
  static inline int clip_ = 0;
};

// M5.Display, as much of it as the firmware touches.
struct M5Class {
  M5Canvas Display;
};
extern M5Class M5;
