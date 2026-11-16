#pragma once
#include <stdbool.h>

struct AppConfig {
    char ssid[33];
    char password[65];
    char refresh_token[512];
};

// Loads the saved settings (flash), falling back to compile-time defaults from secrets.h.
// Returns true when a Wi-Fi SSID is known.
bool config_load(AppConfig *out);

// Saves settings to flash. Returns true on success.
bool config_save(const AppConfig *c);
