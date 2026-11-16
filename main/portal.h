#pragma once
#include <stdbool.h>

// Opens the setup hotspot (open network SETUP_AP_SSID) with a captive web form where the
// owner enters Wi-Fi name/password and the Spotify token. Idempotent, returns immediately.
// After the form is submitted the settings are saved and the board reboots.
void portal_start(void);

bool portal_active(void);
