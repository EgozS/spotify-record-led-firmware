#include "screen.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "disc.h"  // DISC_SIZE

// 3x5 font, one byte per row, low 3 bits = pixels (MSB on the left).
static const uint8_t *glyph(char c) {
    static const uint8_t kBlank[5] = {0, 0, 0, 0, 0};
    static const uint8_t kLetters[26][5] = {
        {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7},  // A-E
        {7, 4, 6, 4, 4}, {3, 4, 5, 5, 3}, {5, 5, 7, 5, 5}, {7, 2, 2, 2, 7}, {1, 1, 1, 5, 2},  // F-J
        {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7}, {5, 7, 7, 5, 5}, {7, 5, 5, 5, 5}, {2, 5, 5, 5, 2},  // K-O
        {6, 5, 6, 4, 4}, {2, 5, 5, 6, 3}, {6, 5, 6, 5, 5}, {3, 4, 2, 1, 6}, {7, 2, 2, 2, 2},  // P-T
        {5, 5, 5, 5, 7}, {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5}, {5, 5, 2, 2, 2},  // U-Y
        {7, 1, 2, 4, 7},                                                                        // Z
    };
    static const uint8_t kDigits[10][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {6, 1, 2, 4, 7}, {6, 1, 2, 1, 6}, {5, 5, 7, 1, 1},
        {7, 4, 6, 1, 6}, {3, 4, 7, 5, 7}, {7, 1, 2, 2, 2}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 6},
    };
    static const uint8_t kDot[5] = {0, 0, 0, 0, 2};
    static const uint8_t kDash[5] = {0, 0, 7, 0, 0};
    if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
    if (c >= 'a' && c <= 'z') return kLetters[c - 'a'];
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    if (c == '.') return kDot;
    if (c == '-') return kDash;
    return kBlank;
}

static inline void put(uint8_t *out, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= DISC_SIZE || y >= DISC_SIZE) return;
    uint8_t *p = &out[(y * DISC_SIZE + x) * 3];
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

// Draws centered text (4 px per character). Returns nothing; clipped to the panel.
static void text_centered(uint8_t *out, int y, const char *s, uint8_t r, uint8_t g, uint8_t b) {
    int n = (int)strlen(s);
    int x0 = (DISC_SIZE - (n * 4 - 1)) / 2;
    for (int i = 0; i < n; i++) {
        const uint8_t *gl = glyph(s[i]);
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                if (gl[row] & (4 >> col)) put(out, x0 + i * 4 + col, y + row, r, g, b);
            }
        }
    }
}

void screen_setup_render(uint32_t t_ms, uint8_t scale, uint8_t *out) {
    memset(out, 0, DISC_SIZE * DISC_SIZE * 3);

    // Wi-Fi symbol: a dot plus three arcs that light up one after another.
    const int phase = (t_ms / 450) % 4;  // 0: dot, 1..3: arcs
    const float cx = 31.5f, cy = 27.0f;
    const float radii[3] = {7.0f, 13.0f, 19.0f};
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < DISC_SIZE; x++) {
            float dx = x - cx, dy = cy - y;
            float r = sqrtf(dx * dx + dy * dy);
            if (r < 2.2f) {
                put(out, x, y, 30, 215, 96);
                continue;
            }
            if (dy <= 0) continue;
            float ang = atan2f(dx, dy);  // 0 = straight up
            if (fabsf(ang) > 0.82f) continue;
            for (int a = 0; a < 3; a++) {
                if (fabsf(r - radii[a]) < 1.0f) {
                    if (phase > a) put(out, x, y, 30, 215, 96);
                    else put(out, x, y, 14, 30, 20);
                }
            }
        }
    }

    text_centered(out, 33, "JOIN WIFI", 235, 235, 235);
    text_centered(out, 41, SETUP_AP_SSID, 30, 215, 96);
    text_centered(out, 51, "192.168.4.1", 130, 130, 130);

    for (int i = 0; i < DISC_SIZE * DISC_SIZE * 3; i++) out[i] = (uint8_t)((out[i] * scale) / 255);
}
