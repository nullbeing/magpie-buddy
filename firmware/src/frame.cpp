// magpie's frame, parsed. One body, fixed-size, no allocation per frame.
#include "magpie.h"

#include <ArduinoJson.h>

namespace magpie {
namespace {

// copy is strlcpy for a fixed field: cut on a UTF-8 boundary, so a name cut
// short is still a name and not half a character.
void copy(char* dst, size_t cap, const char* src) {
  if (!src) {
    dst[0] = 0;
    return;
  }
  size_t n = strlen(src);
  if (n >= cap) {
    n = cap - 1;
    // step back off a continuation byte: 10xxxxxx
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) n--;
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

}  // namespace

bool parse(const char* body, Frame& f, const char** why) {
  // the board's parser is fixed-size: the frame is 2048 at most, and a doc
  // that needs more is not one this display reads
  JsonDocument doc;
  auto err = deserializeJson(doc, body);
  if (err) {
    *why = err.c_str();
    return false;
  }
  if (!doc.is<JsonObject>()) {
    *why = "not an object";
    return false;
  }

  f.v = doc["v"] | 0;
  if (f.v <= 0) {
    *why = "no version";
    return false;
  }
  f.seq = doc["seq"].as<int64_t>();
  f.now = doc["now"].as<int64_t>();
  f.up = (doc["up"] | 0) != 0;

  f.hasRoute = false;
  if (JsonObjectConst r = doc["r"]; !r.isNull()) {
    Route& x = f.route;
    x.id = r["id"].as<int64_t>();
    copy(x.agent, sizeof x.agent, r["agent"] | "");
    copy(x.provider, sizeof x.provider, r["provider"] | "");
    copy(x.model, sizeof x.model, r["model"] | "");
    copy(x.group, sizeof x.group, r["group"] | "");
    x.try_ = r["try"] | 0;
    x.tries = r["tries"] | 0;
    x.moved = r["moved"] | false;
    x.done = r["done"] | false;
    x.status = r["status"] | 0;
    x.ms = r["ms"].as<int64_t>();
    x.ttft = r["ttft"].as<int64_t>();
    x.fail = r["fail"] | false;
    f.hasRoute = x.present();
  }

  f.hasContext = false;
  if (JsonObjectConst c = doc["c"]; !c.isNull()) {
    f.context.tokens = c["tok"] | 0;
    f.context.window = c["win"] | 0;
    f.context.cache = c["cache"] | 0;
    f.context.out = c["out"] | 0;
    // parts is an object of kind -> tokens, the kinds in magpie's own order.
    // A frame that names none leaves hasParts false, and the board draws the
    // used/free split without colours rather than inventing a breakdown.
    f.context.hasParts = false;
    for (int i = 0; i < kPartCount; i++) f.context.parts[i] = 0;
    if (JsonObjectConst parts = c["parts"]; !parts.isNull()) {
      static const char* const kNames[kPartCount] = {"system", "tools", "memory",
                                                    "files", "results", "chat"};
      for (int i = 0; i < kPartCount; i++) {
        int n = parts[kNames[i]] | 0;
        f.context.parts[i] = n;
        if (n > 0) f.context.hasParts = true;
      }
    }
    f.hasContext = true;
  }

  f.hasUsage = false;
  if (JsonObjectConst u = doc["u"]; !u.isNull()) {
    f.usage.requests = u["req"] | 0;
    f.usage.tokens = u["tok"].as<int64_t>();
    f.usage.errors = u["err"] | 0;
    // the four kinds of token the window's cards split out. A pusher that
    // sends none leaves them at zero and the page says what it can.
    f.usage.in = u["in"].as<int64_t>();
    f.usage.out = u["out"].as<int64_t>();
    f.usage.cacheRead = u["cr"].as<int64_t>();
    f.usage.reasoning = u["rs"].as<int64_t>();
    // a cost of 0 is a real answer (a free model); absent is "unknown", and
    // the screen then shows no money rather than $0.00
    if (u["cost"].is<float>()) {
      f.usage.cost = u["cost"] | 0.0f;
      f.usage.hasCost = true;
    }
    f.hasUsage = true;
  }

  f.nbars = 0;
  if (JsonArrayConst s = doc["s"]; !s.isNull()) {
    for (JsonObjectConst b : s) {
      if (f.nbars >= kBars) break;
      Bar& bar = f.bars[f.nbars++];
      snprintf(bar.label, sizeof bar.label, "%s", b["l"] | "");
      bar.v = b["v"].as<int64_t>();
      bar.c = b["c"].as<int64_t>();
    }
  }

  f.cards = 0;
  if (JsonArrayConst q = doc["q"]; !q.isNull()) {
    for (JsonObjectConst c : q) {
      if (f.cards >= kCards) break;
      Quota& card = f.quotas[f.cards++];
      copy(card.name, sizeof card.name, c["n"] | "");
      copy(card.sub, sizeof card.sub, c["p"] | "");
      copy(card.disp, sizeof card.disp, c["d"] | "");
      card.left = c["left"] | 0;
      if (card.left < 0) card.left = 0;
      if (card.left > 100) card.left = 100;
      card.at = c["at"].as<int64_t>();
    }
  }
  return true;
}

}  // namespace magpie
