#pragma once
#include <stdint.h>

// "Setup mode" screen: animated Wi-Fi symbol + hotspot name + address (64x64 RGB888).
// `scale` 0..255 dims every channel like the record does.
void screen_setup_render(uint32_t t_ms, uint8_t scale, uint8_t *out);

// Counter screen: just the number of days, in smooth glowing digits ("---" until the clock is set).
void screen_counter_render(uint32_t t_ms, int days, uint8_t scale, uint8_t *out);

// Firmware update progress screen (percent 0..100).
void screen_update_render(uint32_t t_ms, int percent, uint8_t scale, uint8_t *out);
