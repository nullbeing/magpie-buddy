// The board's own network setup: with nothing in NVS it becomes a small access
// point, serves one page, and takes the network it is given.
//
// Nothing here talks to magpie. This is how the board gets onto a network in
// the first place — the one step that used to need a rebuild, because the SSID
// and password were compiled in from config.h.
#pragma once

#include <Arduino.h>

namespace magpie {
namespace provision {

// hasCredentials is whether there is a network to join: one saved in NVS, or
// the one compiled in from config.h, which is what an older build has.
bool hasCredentials();

// ssid and pass are what to join. Empty when nothing is saved and config.h
// names nothing.
const char* ssid();
const char* pass();

// save writes a network to NVS. The portal calls it, and so does the serial
// `join` command, which is how a board with no page to be reached on recovers.
void save(const char* ssid, const char* pass);

// requestSetup asks the next boot to come up in setup mode, keeping the saved
// network. That is what the three-second hold does: looking at the setup page
// should not cost you the network you already had.
void requestSetup();

// takeSetupRequest reads that request and clears it, so it is acted on once:
// a board that was power-cycled while in setup comes back to its pages.
bool takeSetupRequest();

// forget clears the saved network, so the next boot provisions again. Only the
// serial command does this; the gesture does not.
void forget();

// dump prints what NVS holds, without the password itself.
void dump();

// apName is the access point this board raises while it is being set up.
const char* apName();

// begin raises the access point and starts the captive portal. Call it only
// when hasCredentials() is false: it takes the radio away from the station.
void begin();

// tick serves the portal. It is a no-op before begin(), and after the board has
// been given a network it answers the page and then restarts into it.
void tick();

}  // namespace provision
}  // namespace magpie
