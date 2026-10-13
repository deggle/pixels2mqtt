#pragma once

#include "settings.h"

// HTTP server on port 80: settings page (captive portal in setup mode) and a
// status page in station mode. `s` must outlive the server; saving updates it.
void webBegin(Settings& s);
void webLoop();  // handles the deferred reboot after saving settings
