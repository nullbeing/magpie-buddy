#include "provision.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "setup_page.h"

// The network an older build compiled in. A board flashed from config.h still
// joins it, so the file is read here too; a checkout without one is a board
// that must be set up over the air, which is what this file is for.
#if __has_include("config.h")
#include "config.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

// The network the board joins, saved where a power cut cannot lose it.
//
// A board that has never been set up raises its own access point and serves one
// page; joining that page's network is the whole configuration. That is the
// step that used to need a rebuild, because config.h compiled the SSID in.
namespace magpie {
namespace provision {
namespace {

// Same namespace as the theme, so one board's settings sit together in one
// 20 KB NVS partition.
constexpr const char* kNvs = "magpie";
constexpr const char* kKeySsid = "wifi_ssid";
constexpr const char* kKeyPass = "wifi_pass";
// Set when the user asks the board to forget: it suppresses the network
// compiled in from config.h too, which a plain "no keys saved" cannot.
constexpr const char* kKeyForgotten = "wifi_forgot";

constexpr uint16_t kPortalPort = 80;
constexpr uint8_t kMaxNetworks = 24;

Preferences prefs;
DNSServer dns;
WebServer portal(kPortalPort);

bool started = false;
bool saved = false;         // a network was just written; the board is leaving
uint32_t savedAt = 0;
char apNameBuf[32] = {0};

// The captive portal probes phones make when they join a network. Answering any
// of them with the page is what makes the sheet appear by itself on iOS and
// Android; a 404 leaves the user typing an address they were never told.
const char* const kProbes[] = {
    "/generate_204",          // Android
    "/gen_204",               // Android, older
    "/hotspot-detect.html",   // iOS and macOS
    "/library/test/success.html",
    "/connecttest.txt",       // Windows
    "/redirect",              // Windows
    "/canonical.html",        // Firefox
    "/success.txt",
};

bool isProbe(const String& uri) {
  for (const char* p : kProbes) {
    if (uri == p) return true;
  }
  return false;
}

void sendPage() {
  portal.sendHeader("Cache-Control", "no-store");
  portal.send_P(200, "text/html; charset=utf-8", kSetupPage);
}

// redirectToPage answers a probe with a redirect to the page, which is how a
// captive portal sheet is opened.
void redirectToPage() {
  portal.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  portal.send(302, "text/plain", "");
}

void handleScan() {
  // The radio is in AP+station so a scan can run without dropping the access
  // point the phone is on.
  const int found = WiFi.scanNetworks(false, true);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < found && arr.size() < kMaxNetworks; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  portal.sendHeader("Cache-Control", "no-store");
  portal.send(200, "application/json", out);
}

void handleSave() {
  String ssid = portal.arg("ssid");
  String pass = portal.arg("password");
  ssid.trim();
  if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63) {
    portal.send(400, "text/plain", "a network name is required, up to 32 characters");
    return;
  }
  save(ssid.c_str(), pass.c_str());
  Serial.printf("setup: saved %s\n", ssid.c_str());
  portal.send(200, "text/plain", "saved");
  saved = true;
  savedAt = millis();
}

}  // namespace

void dump() {
  const bool ok = prefs.begin(kNvs, true);
  const String s = ok ? prefs.getString(kKeySsid, "") : String("");
  const String p = ok ? prefs.getString(kKeyPass, "") : String("");
  const bool f = ok ? prefs.getBool(kKeyForgotten, false) : false;
  prefs.end();
  Serial.printf("setup: open=%d ssid=\"%s\" pass=%u chars forgotten=%d\n", int(ok),
                s.c_str(), unsigned(p.length()), int(f));
}

bool hasCredentials() {
  prefs.begin(kNvs, true);
  const bool forgotten = prefs.getBool(kKeyForgotten, false);
  const String s = prefs.getString(kKeySsid, "");
  prefs.end();
  if (forgotten) return false;
  if (!s.isEmpty()) return true;
  Serial.printf("setup: nvs says ssid=%s forgotten=%d, config.h ssid=%s\n",
                s.c_str(), int(forgotten), WIFI_SSID);
  // a board built from config.h before this existed still has its network
  return strlen(WIFI_SSID) > 0;
}

const char* ssid() {
  static char buf[33];
  prefs.begin(kNvs, true);
  const bool forgotten = prefs.getBool(kKeyForgotten, false);
  const String s = prefs.getString(kKeySsid, "");
  prefs.end();
  if (forgotten) {
    buf[0] = 0;
  } else if (!s.isEmpty()) {
    snprintf(buf, sizeof buf, "%s", s.c_str());
  } else {
    snprintf(buf, sizeof buf, "%s", WIFI_SSID);
  }
  return buf;
}

const char* pass() {
  static char buf[64];
  prefs.begin(kNvs, true);
  const bool forgotten = prefs.getBool(kKeyForgotten, false);
  const String p = prefs.getString(kKeyPass, "");
  prefs.end();
  if (forgotten) {
    buf[0] = 0;
  } else if (!p.isEmpty()) {
    snprintf(buf, sizeof buf, "%s", p.c_str());
  } else {
    snprintf(buf, sizeof buf, "%s", WIFI_PASS);
  }
  return buf;
}

void save(const char* ssid, const char* pass) {
  const bool ok = prefs.begin(kNvs, false);
  const size_t a = ok ? prefs.putString(kKeySsid, ssid) : 0;
  const size_t b = ok ? prefs.putString(kKeyPass, pass) : 0;
  if (ok) prefs.remove(kKeyForgotten);
  prefs.end();
  Serial.printf("setup: saving %s open=%d wrote ssid=%u pass=%u\n", ssid, int(ok),
                unsigned(a), unsigned(b));
}

void forget() {
  Serial.println("setup: forgetting the saved network");
  prefs.begin(kNvs, false);
  prefs.remove(kKeySsid);
  prefs.remove(kKeyPass);
  prefs.putBool(kKeyForgotten, true);
  prefs.end();
  Serial.println("setup: network forgotten");
}

const char* apName() { return apNameBuf; }

void begin() {
  // A name a phone's Wi-Fi list can tell apart when two boards are on a desk.
  // The number comes from the chip's eFuse, not from WiFi.macAddress(): that one
  // is all zeros until an interface has been started, which is why the first
  // build raised "Magpie-Buddy-0000" on every board.
  const uint64_t chip = ESP.getEfuseMac();
  snprintf(apNameBuf, sizeof apNameBuf, "Magpie-Buddy-%02X%02X",
           static_cast<unsigned>((chip >> 16) & 0xFF),
           static_cast<unsigned>((chip >> 24) & 0xFF));

  // The station half is what scans for networks to offer, but it has a memory
  // of its own: WiFi.begin() stores the network in the ESP-IDF's own NVS, and a
  // station that starts with one stored joins it by itself. That is how a board
  // that had just been told to forget came back on the old network — the
  // station reconnected underneath the portal. So the driver's copy is dropped
  // here: this board keeps its network in the "magpie" namespace, and only
  // there.
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apNameBuf);  // open: the point is to be joined, not to be secure
  delay(200);
  Serial.printf("setup: access point %s at %s\n", apNameBuf,
                WiFi.softAPIP().toString().c_str());

  // Every name resolves to this board, which is what makes the phone decide the
  // network is a captive portal.
  dns.start(53, "*", WiFi.softAPIP());

  portal.on("/", HTTP_GET, sendPage);
  portal.on("/scan", HTTP_GET, handleScan);
  portal.on("/save", HTTP_POST, handleSave);
  portal.onNotFound([]() {
    if (isProbe(portal.uri())) {
      redirectToPage();
      return;
    }
    // any other name still lands on the page: the DNS above answers everything,
    // so a request that arrives is one the user meant for this board
    sendPage();
  });
  portal.begin();
  started = true;
}

void tick() {
  if (!started) return;
  dns.processNextRequest();
  portal.handleClient();
  // a saved network is taken by rebooting into it: the station half has been
  // idle while this ran, and a fresh join is what the user is watching for
  if (saved && millis() - savedAt > 800) {
    Serial.println("setup: restarting into the saved network");
    delay(120);
    ESP.restart();
  }
}

}  // namespace provision
}  // namespace magpie
