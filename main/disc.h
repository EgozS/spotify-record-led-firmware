#pragma once
#include <stdint.h>

#define DISC_SIZE 64

void disc_init(void);

// art: 64x64 RGB888, or nullptr to use the plain idle label.
void disc_set_art(const uint8_t *art);

// Renders one frame into out (64*64*3 RGB888). `angle` in radians (label rotation),
// `scale` 0..255 multiplies every channel (used for very low dimming).
void disc_render(float angle, uint8_t scale, uint8_t *out);
