// Text, drawn from the baked Inter atlases.
//
// M5GFX's own fonts are 1-bit: a glyph is a mask of on-or-off pixels, so every
// curve is a staircase. That is why the board's text looked rough whichever
// face it was given — it is the format, not the typeface.
//
// These atlases keep 4 bits of coverage a pixel (tools/gen-font.py), and this
// draws a glyph by blending it against what is already under it. The same
// header is compiled into the firmware and into tools/render, so the preview
// shows the board's text and not the browser's.
//
// It is a template on the canvas so both canvases fit: the board's M5Canvas and
// the shim's. Both have readPixel, drawPixel, color565, width and height.
#pragma once

#include <stdint.h>
#include <string.h>

#include "ui_font.h"

namespace ui {

// Align is where a text's anchor sits in its line.
enum Align : uint8_t { kLeft, kCenter, kRight };

// coverage is one glyph pixel's alpha, 0..15: two pixels a byte, high nibble
// first, as gen-font.py wrote them.
inline int coverage(const Face& f, const Glyph& g, int x, int y) {
  if (x < 0 || y < 0 || x >= g.w || y >= g.h) return 0;
  const int i = y * g.w + x;
  const uint8_t b = f.alpha[g.off + (i >> 1)];
  return (i & 1) ? (b & 0x0F) : (b >> 4);
}

// glyphOf finds a codepoint's glyph, or null. The atlases are ASCII and
// sorted, so this is a scan; 95 entries is nothing beside the blit.
inline const Glyph* glyphOf(const Face& f, uint16_t cp) {
  for (int i = 0; i < f.count; i++) {
    if (f.glyphs[i].cp == cp) return &f.glyphs[i];
  }
  return nullptr;
}

// width is what a string measures: the sum of its advances.
inline int width(const Face& f, const char* s) {
  int w = 0;
  for (; s && *s; ++s) {
    if (const Glyph* g = glyphOf(f, static_cast<uint8_t>(*s))) w += g->adv;
  }
  return w;
}

// blendPixel puts colour over what is there, by a of 15.
template <class C>
inline void blendPixel(C& c, int x, int y, uint16_t colour, int a) {
  if (a <= 0) return;
  if (x < 0 || y < 0 || x >= c.width() || y >= c.height()) return;
  if (a >= 15) {
    c.drawPixel(x, y, colour);
    return;
  }
  const uint16_t under = c.readPixel(x, y);
  const int r1 = ((colour >> 11) & 0x1F) << 3, g1 = ((colour >> 5) & 0x3F) << 2, b1 = (colour & 0x1F) << 3;
  const int r0 = ((under >> 11) & 0x1F) << 3, g0 = ((under >> 5) & 0x3F) << 2, b0 = (under & 0x1F) << 3;
  c.drawPixel(x, y, c.color565((r1 * a + r0 * (15 - a)) / 15,
                               (g1 * a + g0 * (15 - a)) / 15,
                               (b1 * a + b0 * (15 - a)) / 15));
}

// draw puts s at x on the baseline y, in colour, aligned as asked.
template <class C>
inline void draw(C& c, const Face& f, const char* s, int x, int y, uint16_t colour, Align a = kLeft) {
  if (!s || !*s) return;
  int pen = x;
  if (a == kCenter) pen -= width(f, s) / 2;
  else if (a == kRight) pen -= width(f, s);
  for (; *s; ++s) {
    const Glyph* g = glyphOf(f, static_cast<uint8_t>(*s));
    if (!g) continue;
    for (int gy = 0; gy < g->h; gy++) {
      for (int gx = 0; gx < g->w; gx++) {
        const int cov = coverage(f, *g, gx, gy);
        if (cov) blendPixel(c, pen + g->xoff + gx, y + g->yoff + gy, colour, cov);
      }
    }
    pen += g->adv;
  }
}

// drawTop puts s with its line's top at y, which is what a layout wants: a
// baseline moves when the face changes, a top does not.
template <class C>
inline void drawTop(C& c, const Face& f, const char* s, int x, int y, uint16_t colour, Align a = kLeft) {
  draw(c, f, s, x, y + f.ascent, colour, a);
}

// height is the face's line height: what a row of it takes.
inline int height(const Face& f) { return f.height; }

}  // namespace ui
