#include "disc.h"

#include <math.h>
#include <algorithm>
#include <stdlib.h>
#include <string.h>

// Geometry (pixel units, panel center at 31.5, 31.5)
static const float CX = 31.5f;
static const float CY = 31.5f;
static const float R_DISC = 31.0f;    // 1 px black margin around the record
static const float R_LABEL = 21.5f;
static const float R_HOLE = 1.8f;

static uint8_t s_static[DISC_SIZE * DISC_SIZE * 3];   // vinyl layer (rotation invariant)
static uint8_t s_label_cov[DISC_SIZE * DISC_SIZE];    // 0..255 coverage of the label
static uint8_t s_art[DISC_SIZE * DISC_SIZE * 3];
static bool s_has_art = false;

// The light sheen on the vinyl is part of the record, so it rotates with it.
// Each pixel stores its angle (0..255 = 0..360 deg) and how much sheen it can receive;
// a 256-entry lookup table holds the two-wedge brightness pattern.
static uint8_t s_theta_q[DISC_SIZE * DISC_SIZE];
static uint8_t s_sheen_gain[DISC_SIZE * DISC_SIZE];
static uint8_t s_sheen_lut[256];

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

void disc_init(void) {
    // Two opposite soft wedges, peak 55 brightness, rotated 45 deg from the record's zero angle
    for (int k = 0; k < 256; k++) {
        float a = k * (2.0f * (float)M_PI / 256.0f);
        float s = cosf(2.0f * (a - 0.785f));
        s = s > 0 ? s : 0;
        s = s * s * s * s * s * s;
        s_sheen_lut[k] = (uint8_t)(55.0f * s);
    }
    for (int y = 0; y < DISC_SIZE; y++) {
        for (int x = 0; x < DISC_SIZE; x++) {
            float dx = x - CX, dy = y - CY;
            float r = sqrtf(dx * dx + dy * dy);
            int i = y * DISC_SIZE + x;

            // Disc silhouette with soft edge
            float disc_a = clamp01(R_DISC - r + 0.5f);

            // Vinyl grooves: alternating bands, a smooth ring near the label
            float v = 16.0f;
            if (r > R_LABEL + 2.0f) {
                int band = (int)(r * 1.6f);
                v += (band & 1) ? 7.0f : 0.0f;
                v += 4.0f * sinf(r * 5.1f);
            } else {
                v += 10.0f;  // lead-in ring
            }

            // Sheen: remember this pixel's angle and how much sheen it gets (none under the label)
            float theta = atan2f(dy, dx);  // -pi..pi
            float t = theta / (2.0f * (float)M_PI);
            if (t < 0) t += 1.0f;
            s_theta_q[i] = (uint8_t)((int)(t * 256.0f) & 255);
            s_sheen_gain[i] = (uint8_t)(255.0f * disc_a * clamp01((r - R_LABEL + 1.0f) / 4.0f));

            float lum = v * disc_a;
            uint8_t l = (uint8_t)(lum > 255 ? 255 : lum);
            s_static[i * 3 + 0] = l;
            s_static[i * 3 + 1] = l;
            s_static[i * 3 + 2] = (uint8_t)std::min(255.0f, lum * 1.08f);  // faint cool tint

            // Label coverage (anti-aliased), with the spindle hole punched out
            float cov = clamp01(R_LABEL - r + 0.5f) * clamp01(r - R_HOLE + 0.5f);
            s_label_cov[i] = (uint8_t)(cov * 255.0f);
        }
    }
}

void disc_set_art(const uint8_t *art) {
    if (art) {
        memcpy(s_art, art, sizeof(s_art));
        s_has_art = true;
    } else {
        s_has_art = false;
    }
}

static inline void sample_art(float u, float v, uint8_t *rgb) {
    if (u < 0) u = 0;
    if (v < 0) v = 0;
    if (u > DISC_SIZE - 1.001f) u = DISC_SIZE - 1.001f;
    if (v > DISC_SIZE - 1.001f) v = DISC_SIZE - 1.001f;
    int x0 = (int)u, y0 = (int)v;
    float fx = u - x0, fy = v - y0;
    const uint8_t *p00 = &s_art[(y0 * DISC_SIZE + x0) * 3];
    const uint8_t *p10 = p00 + 3;
    const uint8_t *p01 = p00 + DISC_SIZE * 3;
    const uint8_t *p11 = p01 + 3;
    for (int c = 0; c < 3; c++) {
        float top = p00[c] + (p10[c] - p00[c]) * fx;
        float bot = p01[c] + (p11[c] - p01[c]) * fx;
        rgb[c] = (uint8_t)(top + (bot - top) * fy);
    }
}

void disc_render(float angle, uint8_t scale, uint8_t *out) {
    const float ca = cosf(angle), sa = sinf(angle);
    const float k = 32.0f / R_LABEL;  // label circle spans the whole cover (corners cropped)
    // Record angle in 1/256 turns: the sheen pattern turns by the same amount as the label.
    const int aq = (int)(angle * (256.0f / (2.0f * (float)M_PI))) & 255;

    for (int y = 0; y < DISC_SIZE; y++) {
        for (int x = 0; x < DISC_SIZE; x++) {
            int i = y * DISC_SIZE + x;
            uint8_t rgb[3] = {s_static[i * 3], s_static[i * 3 + 1], s_static[i * 3 + 2]};
            uint8_t sg = s_sheen_gain[i];
            if (sg) {
                int add = (s_sheen_lut[(uint8_t)(s_theta_q[i] - aq)] * sg) / 255;
                rgb[0] = (uint8_t)std::min(255, rgb[0] + add);
                rgb[1] = (uint8_t)std::min(255, rgb[1] + add);
                rgb[2] = (uint8_t)std::min(255, rgb[2] + add + add / 12);
            }
            uint8_t cov = s_label_cov[i];
            if (cov) {
                float dx = x - CX, dy = y - CY;
                float rx = dx * ca + dy * sa;
                float ry = -dx * sa + dy * ca;
                uint8_t lab[3];
                if (s_has_art) {
                    sample_art(CX + rx * k, CY + ry * k, lab);
                } else {
                    // Idle label: Spotify-ish green with a rotating marker dot
                    lab[0] = 20; lab[1] = 150; lab[2] = 70;
                    float mx = rx - 12.0f, my = ry;
                    if (mx * mx + my * my < 9.0f) { lab[0] = 230; lab[1] = 240; lab[2] = 230; }
                }
                for (int c = 0; c < 3; c++) rgb[c] = (uint8_t)((rgb[c] * (255 - cov) + lab[c] * cov) / 255);
            }
            out[i * 3 + 0] = (uint8_t)((rgb[0] * scale) / 255);
            out[i * 3 + 1] = (uint8_t)((rgb[1] * scale) / 255);
            out[i * 3 + 2] = (uint8_t)((rgb[2] * scale) / 255);
        }
    }
}
