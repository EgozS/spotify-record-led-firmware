#pragma once
#include <stdbool.h>
#include <stdint.h>

struct AppConfig {
    char ssid[33];
    char password[65];
    char refresh_token[512];
    int32_t tz_min;     // local time zone, minutes east of UTC (decides when the counter's day changes)
};

// Loads the saved settings (flash), falling back to compile-time defaults.
// Returns true when a Wi-Fi SSID is known.
bool config_load(AppConfig *out);

// Saves settings to flash. Returns true on success.
bool config_save(const AppConfig *c);
