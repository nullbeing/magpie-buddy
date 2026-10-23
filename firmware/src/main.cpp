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

#include "leds.h"
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
// set when there is a saved network behind the setup page, which is what makes
// a tap leave it rather than being swallowed
bool gSetupCanLeave = false;
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
  // Whether this board has a strip at all, and what it is saying. A bare CoreS3
  // and a StackChan answer the same /health otherwise, and a light that cannot
  // be seen from here has to be readable from here instead.
  out += ",\"strip\":";
  out += magpie::leds.present() ? "true" : "false";
  out += ",\"led\":\"";
  out += magpie::leds.present() ? magpie::leds.stateName() : "none";
  out += "\"";
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
  // Opened, read, closed. A handle left open on a namespace is what makes a
  // later write to the same namespace fail, and provision.cpp writes there.
  const bool ok = gPrefs.begin("magpie", false);
  if (!ok) Serial.println("nvs: the theme namespace did not open");
  screen.setTheme(gPrefs.getBool("dark", true));
  gPrefs.end();
}

void saveTheme() {
  if (!screen.themeDirty()) return;
  gPrefs.putBool("dark", screen.darkTheme());
  screen.clearThemeDirty();
  Serial.printf("theme: %s (saved)\n", screen.darkTheme() ? "dark" : "light");
}

// The strip says what the page draws, in the same order of precedence: a board
// with no network is telling you that, whatever the last frame said.
//
// A landing and a failure are said once per route, not once per loop. The flash
// decays and the strip goes back to idle; without this the next pass would see
// route.done still true and start the flash again, so a finished request would
// blink for ever. Which route was announced is kept here because that is the
// caller's own knowledge — the screen treats a flight as over, not as news.
void updateLeds(uint32_t now, bool down) {
  static int64_t announced = 0;
  static int announcedTry = 0;

  if (gProvisioning) {
    magpie::leds.set(magpie::Led::kSetup, 0);
  } else if (down) {
    magpie::leds.set(magpie::Led::kOffline, 0);
  } else if (screen.flying()) {
    magpie::leds.set(magpie::Led::kBusy, screen.routeHue());
  } else {
    const magpie::Frame& f = screen.frame();
    const bool fresh = f.hasRoute && f.route.done &&
                       (f.route.id != announced || f.route.try_ != announcedTry);
    if (fresh) {
      announced = f.route.id;
      announcedTry = f.route.try_;
      magpie::leds.set(f.route.fail ? magpie::Led::kFail : magpie::Led::kOk, 0);
    } else {
      magpie::leds.set(magpie::Led::kIdle, 0);
    }
  }
  magpie::leds.tick(now);
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
  // 3 s, and only a finger that stayed down: M5Unified times it, so a tap
  // cannot be mistaken for a hold.
  M5.Touch.setHoldThresh(3000);
  loadTheme();

  // The strip, if this board has one. A bare CoreS3 has no expander at 0x6F and
  // begin() says so; there is nothing else to do about a board without one.
  const bool strip = magpie::leds.begin();
  Serial.printf("strip: %s\n", strip ? "twelve lights on the IO expander" : "none on this board");

  // No network saved: the board has to be told one, so it raises its own access
  // point and waits for a phone. This is the one step that used to need a
  // rebuild, because the SSID and password were compiled in.
  const bool saved = provision::hasCredentials();
  // A hold asks for the setup page; a board with nothing saved has no choice.
  // The request is taken (and cleared) here, so a board that was power-cycled
  // while it was in setup comes back to its pages rather than staying there.
  const bool asked = provision::takeSetupRequest();
  Serial.printf("boot: %s%s\n", saved ? "a network is saved" : "no network saved",
                asked ? ", setup was asked for" : "");
  if (asked || !saved) {
    provision::begin();
    screen.setup(provision::apName(), WiFi.softAPIP().toString().c_str(), saved);
    gProvisioning = true;
    gSetupCanLeave = saved;
    // The strip says the access point is up before the panel has finished
    // drawing: a board on a shelf is looked at, not read.
    magpie::leds.set(magpie::Led::kSetup, 0);
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
  // The gesture is a hold, and M5Unified already times holds: setHoldThresh
  // below makes wasHold() fire once, 3 s after a finger lands, and it counts
  // only a finger that stayed down. A hand-rolled timer here read "now minus
  // when I first saw a finger" instead, so a touch that arrived while loop()
  // was blocked — during the join at boot — measured against a stale start and
  // wiped the network on a single tap.
  if (M5.Touch.getDetail(0).wasHold()) {
    // Asking for setup, not forgetting: the network stays saved, so the page
    // can be looked at and left again. Only the serial `forget` throws it away.
    provision::requestSetup();
    Serial.println("setup: held three seconds, restarting into setup");
    delay(120);
    ESP.restart();
  }

  // And the same over the serial port, which is how it is tested without a
  // finger, and how a board whose network is gone is brought back.
  {
    static char line[64];
    static size_t len = 0;
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
        if (len && !strcmp(line, "status")) provision::dump();
        // setup: what the three-second hold does, without a finger. It is the
        // same call, so it is how that path is tested.
        if (len && !strcmp(line, "setup")) {
          provision::requestSetup();
          Serial.println("setup: restarting into setup");
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
  }

  if (gProvisioning) {
    // The portal takes the radio; the panel is still this program's to draw.
    // It has to be ticked here, not skipped: the setup page is a page like any
    // other, and screen.setup() only marks it dirty — the drawing happens in
    // tick(). Returning before it left the panel on whatever was on it when the
    // network was forgotten, so the access point was up and the screen said
    // nothing about it.
    provision::tick();
    // A tap leaves the page when there is a saved network behind it. Without
    // one the only way on is a phone, and the footer says so instead.
    if (gSetupCanLeave && M5.Touch.getDetail(0).wasClicked()) {
      Serial.println("setup: leaving setup, the saved network stands");
      delay(120);
      ESP.restart();
    }
    screen.tick(millis());
    updateLeds(millis(), false);
    // Said once: a page that draws and a page that does not look identical from
    // outside the board, and this one was not being drawn at all for a while.
    static bool saidDrawn = false;
    if (!saidDrawn) {
      saidDrawn = true;
      Serial.println("setup: page drawn");
    }
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
  // The strip last, after screen.tick has advanced the flight: the light then
  // says what the page has just been drawn showing, not what it showed a frame
  // ago.
  updateLeds(now, down);
  delay(2);
}
