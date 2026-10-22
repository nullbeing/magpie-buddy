// Copy to config.h and fill in. config.h is gitignored: a Wi-Fi password is
// not something to commit.
//
//   cp firmware/include/config.example.h firmware/include/config.h
//
// A board with no config.h still builds and runs: it joins nothing, shows
// "magpie offline", and logs to the serial port — which is what a first flash
// looks like before the network is filled in.
#pragma once

// Your network. 2.4 GHz only: the ESP32-S3 has no 5 GHz radio, and a 5 GHz-only
// SSID simply isn't seen.
#define WIFI_SSID "your-network"
#define WIFI_PASS "your-password"

// What this display calls itself on the network, so a router's client list has
// something recognisable in it.
#define HOSTNAME "magpie-desk"

// The token the frames must carry, which must equal the plugin's `token`
// setting. It is set here by hand and never sent over the network: discovery
// says where to push, not what the secret is (docs/protocol.md). Empty means
// this display takes a frame from anyone on the local network, and it says so
// on screen.
#define DISPLAY_TOKEN ""

// The port this display serves frames on is the protocol's, 9877: discovery
// answers with that port, so a board that moved it would be unreachable. The
// discovery port is the board's own, and magpie's `discoverPort` setting is
// its: both ends must say the same number.
// #define DISCOVERY_PORT 9878
