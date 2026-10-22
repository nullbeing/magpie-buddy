// magpie's desk display on an M5Stack CoreS3.
//
// It is the receiving half of docs/protocol.md:
//
//   * it broadcasts "MAGPIE-DISPLAY?" to the network until magpie answers with
//     where to push, so nothing has to be typed into the board;
//   * it serves POST /state on 9877, where a frame is drawn;
//   * a frame that stops arriving is "offline" after 45 s on its own, without
//     magpie having to say goodbye.
//
// The screen and the flight are in screen.cpp; the frame is in frame.cpp.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// config.h is the user's own and is gitignored, so a fresh checkout builds
// without it: the board then joins nothing, says so on screen, and logs to the
// serial port. That is what a first flash before the network is filled in
// looks like, and it is better than a compile error.
#if __has_include("config.h")
#include "config.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif
#ifndef HOSTNAME
#define HOSTNAME "magpie-desk"
#endif

#include "magpie.h"
#include "provision.h"
#include "screen.h"

namespace {

// The ports are the protocol's, and both ends may change them; the board's are
// the display's own setting, so a second display on one network can move them.
constexpr uint16_t kHttpPort = 9877;  // the protocol's: discovery answers with it
constexpr uint16_t kDiscoveryPort =
#ifdef DISCOVERY_PORT
    DISCOVERY_PORT;
#else
    9878;
#endif
constexpr const char* kHostname = HOSTNAME;

// How often the board asks who is out there, and how long a silence is
// "magpie is gone". Three heartbeats: magpie sends one every 15 s.
constexpr uint32_t kProbeEvery = 30000;
constexpr uint32_t kOfflineAfter = 45000;

const char* kProbe = "MAGPIE-DISPLAY?";

// the setup portal lives in magpie::provision; the boot code below is global
namespace provision = magpie::provision;

WebServer http(kHttpPort);
// set when the board is being set up: loop() then serves the portal and
// nothing else, because there is no network to be on yet
bool gProvisioning = false;
WiFiUDP udp;
magpie::Screen screen;

// The token the frames must carry, from config.h. It is never taken from the
// network: a probe is broadcast, so an answer that named the token would hand
// it to whoever asked (docs/protocol.md).
const char* kToken =
#ifdef DISPLAY_TOKEN
    DISPLAY_TOKEN;
#else
    "";
#endif
String magpieName;               // what the answer called this computer
String pushURL;                  // where the answer said magpie pushes
uint32_t lastFrame = 0;          // millis() of the last frame
uint32_t lastProbe = 0;
bool everConnected = false;
bool offlineShown = false;

// handleState is POST /state: one whole frame, never a delta.
void handleState() {
  if (kToken[0] && http.header("X-Magpie-Token") != kToken) {
    http.send(401, "text/plain", "wrong or missing X-Magpie-Token");
    return;
  }
  const String body = http.arg("plain");
  if (body.length() > magpie::kMaxBody) {
    // the board's parser is fixed-size: over the limit is a bug in the sender,
    // and saying so is better than drawing half a frame
    http.send(413, "text/plain", "a frame is 2048 bytes at most");
    return;
  }
  magpie::Frame f;
  const char* why = "";
  if (!magpie::parse(body.c_str(), f, &why)) {
    http.send(400, "text/plain", String("not a frame: ") + why);
    return;
  }
  if (f.v > magpie::kFrameVersion) {
    // a newer magpie may add keys; this board draws what it knows and says so
    // on the serial log rather than refusing the frame
    Serial.printf("frame v%d, this board speaks v%d: drawing what it knows\n", f.v, magpie::kFrameVersion);
  }
  screen.set(f);
  lastFrame = millis();
  everConnected = true;
  http.send(200, "text/plain", "");
}

// handleHealth is GET /health: what `magpie display ping` asks.
void handleHealth() {
  const magpie::Frame& f = screen.frame();
  String out = "{\"v\":1,\"object\":\"magpie.display.device\"";
  out += ",\"seq\":" + String((long long)f.seq);
  out += ",\"uptime\":" + String(millis() / 1000);
  out += ",\"rssi\":" + String(WiFi.RSSI());
  out += ",\"magpie\":\"" + magpieName + "\"";
  out += "}";
  http.send(200, "application/json", out);
}

// probe asks the network who pushes, and takes the answer: the address and the
// token are kept, so the next probe is only needed after magpie moves.
void probe(uint32_t now) {
  lastProbe = now;
  udp.beginPacket(IPAddress(255, 255, 255, 255), kDiscoveryPort);
  udp.write(reinterpret_cast<const uint8_t*>(kProbe), strlen(kProbe));
  udp.endPacket();
  // many routers drop the all-ones address; the subnet's own broadcast is the
  // one that gets through, so both go
  const IPAddress bcast = WiFi.broadcastIP();
  if (bcast != IPAddress(255, 255, 255, 255)) {
    udp.beginPacket(bcast, kDiscoveryPort);
    udp.write(reinterpret_cast<const uint8_t*>(kProbe), strlen(kProbe));
    udp.endPacket();
  }
}

// readAnswer takes magpie's reply, if one came.
void readAnswer() {
  const int size = udp.parsePacket();
  if (size <= 0) return;
  char buf[512];
  const int n = udp.read(buf, sizeof buf - 1);
  if (n <= 0) return;
  buf[n] = 0;
  // a display answers a display: only an object of this kind is magpie's
  if (!strstr(buf, "\"magpie.display\"")) return;

  JsonDocument doc;
  if (deserializeJson(doc, buf)) return;
  const char* push = doc["push"] | "";
  if (!push[0]) return;
  pushURL = push;
  magpieName = String(doc["name"] | "");
  // auth says magpie expects a token. A board with none set can't be served,
  // so it says so rather than looking like a network fault.
  const bool auth = doc["auth"] | false;
  Serial.printf("magpie at %s (push %s, token %s)\n", udp.remoteIP().toString().c_str(), push,
                auth ? (kToken[0] ? "set" : "MISSING") : "not used");
  if (auth && !kToken[0]) {
    Serial.println("this magpie wants a token: set DISPLAY_TOKEN in config.h to its `token` setting");
  }
  // magpie pushes at once when it answers a probe, so there is nothing to ask
  // for here: the frame that follows is the first thing this board draws.
}

// The theme choice, kept in NVS so a board that is unplugged comes back the way
// it was left. Not in Screen: the renderer compiles Screen, and it has no flash.
Preferences gPrefs;

void loadTheme() {
  gPrefs.begin("magpie", false);
  screen.setTheme(gPrefs.getBool("dark", true));
}

void saveTheme() {
  if (!screen.themeDirty()) return;
  gPrefs.putBool("dark", screen.darkTheme());
  screen.clearThemeDirty();
  Serial.printf("theme: %s (saved)\n", screen.darkTheme() ? "dark" : "light");
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.println("\nmagpie desk display");
  if (!screen.begin()) {
    Serial.println("the display's sprite didn't allocate: is PSRAM on?");
  }
  loadTheme();

  // No network saved: the board has to be told one, so it raises its own access
  // point and waits for a phone. This is the one step that used to need a
  // rebuild, because the SSID and password were compiled in.
  Serial.printf("boot: %s\n", provision::hasCredentials() ? "a network is saved" : "no network saved");
  if (!provision::hasCredentials()) {
    provision::begin();
    screen.setup(provision::apName(), WiFi.softAPIP().toString().c_str());
    gProvisioning = true;
    Serial.println("setup: waiting for a network from the portal");
    return;
  }

  screen.offline(true);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kHostname);
  WiFi.begin(provision::ssid(), provision::pass());
  Serial.printf("wifi: joining %s", provision::ssid());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("wifi: %s\n", WiFi.localIP().toString().c_str());
    // No modem sleep: a display is pushed to, and a station that naps between
    // beacons answers a frame tens of seconds late. Measured here with sleep on:
    // 671 ms round trip and 60% loss to a board 2 ms from its router, while the
    // router itself lost nothing — the radio was napping, not out of range. A
    // board on a desk is on mains power, so the milliamps it costs are not worth
    // a frame that arrives after the next one replaced it.
    //
    // It is set after the join, not before: begin() resets the station, and a
    // setting made before it does not survive. noSleep() keeps it for the
    // reconnects too, so a dropped link doesn't quietly start napping.
    WiFi.setSleep(false);
  } else {
    // A network was given and did not take. Say which one and what to do about
    // it: "magpie offline" is what this used to show, which looks like magpie's
    // fault and tells nobody that a password was mistyped.
    Serial.printf("wifi: %s did not take\n", provision::ssid());
    screen.setupFailed(provision::ssid());
    // no network is not a crash: the screen says so and keeps trying, because
    // a board on a desk is often plugged in before the router is up
    Serial.println("wifi: not joined yet, retrying in the background");
  }

  udp.begin(kDiscoveryPort);
  http.on("/state", HTTP_POST, handleState);
  http.on("/health", HTTP_GET, handleHealth);
  http.onNotFound([] {
    http.send(404, "text/plain", "magpie's desk display: POST /state, GET /health");
  });
  http.begin();
  Serial.printf("http on :%d, discovery on :%d\n", kHttpPort, kDiscoveryPort);
  probe(millis());
}

void loop() {
  M5.update();

  // Two ways back into setup, because a board on a shelf cannot be reflashed.
  //
  // A finger held for three seconds forgets the network and starts over; that
  // is the only gesture this panel has that is deliberately slow, so nothing
  // else can trigger it by accident. And the same thing over the serial port,
  // spelled "forget", which is how it is tested without a finger.
  {
    static uint32_t heldSince = 0;
    static char line[32];
    static size_t len = 0;
    const uint32_t now0 = millis();
    while (Serial.available()) {
      const char c = Serial.read();
      if (c == '\n' || c == '\r') {
        line[len] = 0;
        if (len && !strcmp(line, "forget")) {
          provision::forget();
          Serial.println("setup: restarting to be set up again");
          delay(120);
          ESP.restart();
        }
        // join <ssid> <password>: the same save the portal makes, over the
        // cable. A board whose network is gone has no page to be reached on,
        // and this is what brings it back without a phone.
        if (len > 5 && !strncmp(line, "join ", 5)) {
          char* sp = strchr(line + 5, ' ');
          if (sp) {
            *sp = 0;
            provision::save(line + 5, sp + 1);
            Serial.println("setup: saved over serial, restarting");
            delay(120);
            ESP.restart();
          } else {
            Serial.println("usage: join <ssid> <password>");
          }
        }
        len = 0;
      } else if (len < sizeof line - 1) {
        line[len++] = c;
      }
    }
    if (M5.Touch.getCount() > 0 && M5.Touch.getDetail(0).isPressed()) {
      if (heldSince == 0) {
        heldSince = now0;
      } else if (now0 - heldSince > 3000) {
        provision::forget();
        Serial.println("setup: held three seconds, restarting to be set up");
        delay(120);
        ESP.restart();
      }
    } else {
      heldSince = 0;
    }
  }

  if (gProvisioning) {
    // the portal takes the radio and the panel; it restarts the board itself
    // once a network is saved
    provision::tick();
    delay(2);
    return;
  }
  http.handleClient();
  readAnswer();

  const uint32_t now = millis();

  // Join again after a drop, without being power-cycled.
  //
  // Only when the station is genuinely idle: WL_DISCONNECTED is the state it
  // sits in with nothing in progress, while the connecting states mean an
  // attempt is already under way. Calling reconnect() during one makes the
  // driver answer "sta is connecting, return error" and throws that attempt
  // away, which is what a board with no network did every 5 s before this
  // check was here.
  //
  // The wait doubles while it keeps failing, so a board whose network isn't
  // there doesn't spend the radio's time on it, and goes back to the short wait
  // once it has been on.
  static uint32_t nextTry = 0, wait = 5000;
  if (WiFi.status() == WL_CONNECTED) {
    screen.setupDone();  // a network that arrived late still counts
    if (wait != 5000) {
      // it has just come back from a drop: keep the radio awake, as at boot
      WiFi.setSleep(false);
    }
    wait = 5000;
    nextTry = 0;
  } else if (WiFi.status() == WL_DISCONNECTED && (nextTry == 0 || (int32_t)(now - nextTry) >= 0)) {
    WiFi.reconnect();
    nextTry = now + wait;
    wait = wait < 60000 ? wait * 2 : 60000;
  }

  // ask again until frames come: every 30 s while nothing has arrived, and
  // after 3 missed heartbeats once it has
  const bool heard = everConnected && now - lastFrame < kOfflineAfter;
  if (WiFi.status() == WL_CONNECTED && (!heard || pushURL.isEmpty())) {
    if (now - lastProbe > kProbeEvery) probe(now);
  }

  // offline is the board's own call, counted from the last frame: magpie may
  // have quit, the network may have gone, and neither sends a goodbye
  const bool down = !everConnected || now - lastFrame > kOfflineAfter;
  if (down != offlineShown) {
    offlineShown = down;
    screen.offline(down);
  }

  // The touch panel, as the three things a finger does. M5.update() has
  // already read it, so this only asks what it saw.
  //
  //   wasPressed   the finger landed          → touchDown
  //   isPressed    it is still down           → touchMove, so the page follows
  //   wasReleased  it lifted                  → touchUp: swipe or tap
  //
  // isPressed, not isDragging. M5Unified's states are a small machine:
  //
  //   touch (0001)  a finger down, still still
  //   flick (1001)  a finger down that has moved past its 8px threshold
  //   drag  (1101)  ... which only a second finger or a hold turns into
  //
  // isDragging() asks for 1101, so a one-finger slide — the usual case — is
  // never a drag and the page never followed it. isPressed() is the bit every
  // "finger is down" state carries.
  //
  // And there is no getCount() gate. It counts the points the panel reports,
  // which is 0 on the frame the finger lifts — the very frame wasReleased()
  // is true on. Gating on it meant touchUp was never called and no tap ever
  // registered.
  const auto& t = M5.Touch.getDetail(0);
  if (t.wasPressed()) {
    screen.touchDown(t.x, t.y);
    Serial.printf("touch: down at %d,%d\n", t.x, t.y);
  } else if (t.isPressed()) {
    screen.touchMove(t.x);
  } else if (t.wasReleased()) {
    screen.touchUp(t.x, t.y);
    Serial.printf("touch: up at %d,%d -> page %d\n", t.x, t.y, int(screen.page()));
  }

  // The pack, every few seconds: the gauge is an I2C part, and a level that
  // moves a percent a minute does not need reading a thousand times a second.
  static uint32_t batteryAt = 0;
  if (now - batteryAt > 5000) {
    batteryAt = now;
    screen.setBattery(M5.Power.getBatteryLevel(),
                      M5.Power.isCharging() == m5::Power_Class::is_charging);
  }

  saveTheme();
  screen.tick(now);
  delay(2);
}
