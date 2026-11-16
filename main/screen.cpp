#include "screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"

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
    static const uint8_t kPercent[5] = {5, 1, 2, 4, 5};
    static const uint8_t kQuestion[5] = {6, 1, 2, 0, 2};
    if (c == '.') return kDot;
    if (c == '-') return kDash;
    if (c == '%') return kPercent;
    if (c == '?') return kQuestion;
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

static void apply_scale(uint8_t *out, uint8_t scale) {
    for (int i = 0; i < DISC_SIZE * DISC_SIZE * 3; i++) out[i] = (uint8_t)((out[i] * scale) / 255);
}

// ---------------------------------------------------------------- Counter: smooth stroke digits
//
// Each digit is a few polylines in a unit box (x right, y down, both 0..1). They are drawn as
// anti-aliased round strokes with a soft glow, which looks far smoother than a pixel font.

#define MAX_PTS 48
struct Poly {
    float x[MAX_PTS], y[MAX_PTS];
    int n = 0;
};
struct StrokeGlyph {
    Poly s[3];
    int ns = 0;
};

static void pt(Poly &p, float x, float y) {
    if (p.n < MAX_PTS) {
        p.x[p.n] = x;
        p.y[p.n] = y;
        p.n++;
    }
}

// Elliptical arc; angles in radians, y down (so increasing angle runs clockwise on screen).
static void arc(Poly &p, float cx, float cy, float rx, float ry, float a0, float a1, int steps) {
    for (int i = 0; i <= steps; i++) {
        float a = a0 + (a1 - a0) * i / steps;
        pt(p, cx + rx * cosf(a), cy + ry * sinf(a));
    }
}

static void build_glyph(char c, StrokeGlyph &g) {
    const float PI_ = (float)M_PI;
    g.ns = 1;
    Poly &a = g.s[0];
    switch (c) {
        case '0':
            arc(a, 0.5f, 0.5f, 0.5f, 0.5f, -PI_ / 2, 3 * PI_ / 2, 30);
            break;
        case '1':
            pt(a, 0.18f, 0.24f); pt(a, 0.58f, 0.0f); pt(a, 0.58f, 1.0f);
            break;
        case '2':
            arc(a, 0.5f, 0.27f, 0.5f, 0.27f, PI_, 2 * PI_ + 0.55f, 18);
            pt(a, 0.0f, 1.0f); pt(a, 1.0f, 1.0f);
            break;
        case '3': {
            arc(a, 0.5f, 0.25f, 0.5f, 0.25f, PI_ + 0.35f, 2 * PI_ + PI_ / 2, 18);
            g.ns = 2;
            arc(g.s[1], 0.5f, 0.75f, 0.5f, 0.25f, -PI_ / 2, PI_ - 0.35f, 18);
            break;
        }
        case '4': {
            pt(a, 0.72f, 0.0f); pt(a, 0.0f, 0.68f); pt(a, 1.0f, 0.68f);
            g.ns = 2;
            pt(g.s[1], 0.72f, 0.22f); pt(g.s[1], 0.72f, 1.0f);
            break;
        }
        case '5':
            pt(a, 0.92f, 0.0f); pt(a, 0.14f, 0.0f); pt(a, 0.08f, 0.47f);
            arc(a, 0.48f, 0.72f, 0.52f, 0.28f, -2.45f, 2.55f, 22);
            break;
        case '6':
            pt(a, 0.92f, 0.06f); pt(a, 0.62f, 0.0f); pt(a, 0.3f, 0.1f); pt(a, 0.08f, 0.38f); pt(a, 0.0f, 0.68f);
            arc(a, 0.5f, 0.68f, 0.5f, 0.32f, PI_, 3 * PI_, 28);
            break;
        case '7':
            pt(a, 0.0f, 0.0f); pt(a, 1.0f, 0.0f); pt(a, 0.36f, 1.0f);
            break;
        case '8': {
            arc(a, 0.5f, 0.25f, 0.42f, 0.25f, -PI_ / 2, 3 * PI_ / 2, 24);
            g.ns = 2;
            arc(g.s[1], 0.5f, 0.745f, 0.5f, 0.255f, -PI_ / 2, 3 * PI_ / 2, 28);
            break;
        }
        case '9': {
            arc(a, 0.5f, 0.3f, 0.5f, 0.3f, -PI_ / 2, 3 * PI_ / 2, 28);
            g.ns = 2;
            Poly &t = g.s[1];
            pt(t, 1.0f, 0.3f); pt(t, 0.97f, 0.62f); pt(t, 0.8f, 0.88f); pt(t, 0.5f, 1.0f); pt(t, 0.2f, 0.96f);
            pt(t, 0.06f, 0.86f);
            break;
        }
        default:  // '-' (clock not set yet)
            pt(a, 0.1f, 0.5f); pt(a, 0.9f, 0.5f);
            break;
    }
}

static inline float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}

// Shapes are computed once per day count; colors are applied every frame so they can drift.
static uint8_t s_cover[DISC_SIZE * DISC_SIZE];  // stroke coverage 0..255
static uint8_t s_glow[DISC_SIZE * DISC_SIZE];   // halo strength 0..255
static uint8_t s_pos[DISC_SIZE * DISC_SIZE];    // position along the color gradient 0..255
static int s_counter_days = -2;                 // days value the cached shapes were drawn for

// Cyclic palette (hot pink -> coral -> peach -> soft violet -> back to pink), 256 entries.
static uint8_t s_pal[256][3];
static bool s_pal_ready = false;

static void build_palette(void) {
    static const float kStops[5][3] = {
        {255, 70, 150},   // hot pink
        {255, 120, 105},  // coral
        {255, 195, 140},  // peach
        {190, 105, 255},  // soft violet
        {255, 70, 150},   // back to pink
    };
    for (int i = 0; i < 256; i++) {
        float x = i * 4.0f / 256.0f;
        int k = (int)x;
        float f = x - k;
        f = f * f * (3.0f - 2.0f * f);  // smoothstep between stops
        for (int c = 0; c < 3; c++) s_pal[i][c] = (uint8_t)(kStops[k][c] + (kStops[k + 1][c] - kStops[k][c]) * f);
    }
    s_pal_ready = true;
}

static void build_counter_image(int days) {
    memset(s_cover, 0, sizeof(s_cover));
    memset(s_glow, 0, sizeof(s_glow));
    memset(s_pos, 0, sizeof(s_pos));
    char num[12];
    if (days >= 0) snprintf(num, sizeof(num), "%d", days);
    else strcpy(num, "---");
    const int n = (int)strlen(num);

    const float gap = n >= 4 ? 3.5f : 4.5f;
    float w = (48.0f - gap * (n - 1)) / n;   // whole number is at most 48 px wide: clear margins on both sides
    if (w > 13.5f) w = 13.5f;
    const float h = n >= 4 ? 25.0f : 29.0f;
    const float hw = h * 0.052f;                       // half the stroke width
    const float total = n * w + (n - 1) * gap;
    const float x_start = (DISC_SIZE - total) / 2.0f;
    const float y_start = (DISC_SIZE - h) / 2.0f;

    for (int i = 0; i < n; i++) {
        StrokeGlyph g;
        build_glyph(num[i], g);
        const float ox = x_start + i * (w + gap) + hw, oy = y_start + hw;
        const float bw = w - 2 * hw, bh = h - 2 * hw;

        const int xa = (int)floorf(x_start + i * (w + gap) - 6), xb = (int)ceilf(x_start + i * (w + gap) + w + 6);
        const int ya = (int)floorf(y_start - 6), yb = (int)ceilf(y_start + h + 6);
        for (int y = ya; y <= yb; y++) {
            for (int x = xa; x <= xb; x++) {
                if (x < 0 || y < 0 || x >= DISC_SIZE || y >= DISC_SIZE) continue;
                const float px = x + 0.5f, py = y + 0.5f;
                float d = 1e9f;
                for (int s = 0; s < g.ns; s++) {
                    const Poly &p = g.s[s];
                    for (int k = 0; k + 1 < p.n; k++) {
                        float dd = seg_dist(px, py, ox + p.x[k] * bw, oy + p.y[k] * bh, ox + p.x[k + 1] * bw,
                                            oy + p.y[k + 1] * bh);
                        if (dd < d) d = dd;
                    }
                }
                if (d > hw + 6.0f) continue;

                float cover = hw + 0.5f - d;                       // anti-aliased stroke
                cover = cover < 0 ? 0 : (cover > 1 ? 1 : cover);
                float over = d > hw ? d - hw : 0;
                float glow = 0.34f * expf(-(over * over) / 8.0f);  // soft halo around the stroke

                int idx = y * DISC_SIZE + x;
                uint8_t cv = (uint8_t)(cover * 255.0f), gl = (uint8_t)(glow * 255.0f);
                if (cv > s_cover[idx]) s_cover[idx] = cv;
                if (gl > s_glow[idx]) s_glow[idx] = gl;
                // Gradient runs diagonally across the whole panel
                float pos = (px * 0.7f + py * 0.55f) / (DISC_SIZE * 1.25f);
                s_pos[idx] = (uint8_t)(pos < 0 ? 0 : (pos > 1 ? 255 : pos * 255.0f));
            }
        }
    }
}

// One full trip through the palette every GRADIENT_PERIOD_MS.
#define GRADIENT_PERIOD_MS 6000

// ---------------------------------------------------------------- Twinkling background stars
//
// Small sparkles that fade in and out at random spots of the empty background. A third of them are
// drawn as a little "plus"; they take their color from the drifting palette, lifted toward white.

#define NUM_STARS 18
struct Star {
    int x, y;
    uint32_t start, life;  // ms
    uint8_t hue;
    bool plus, alive;
};
static Star s_stars[NUM_STARS];
static bool s_stars_ready = false;

static void spawn_star(Star &s, uint32_t now) {
    for (int tries = 0; tries < 30; tries++) {
        int x = 1 + (int)(esp_random() % 62), y = 1 + (int)(esp_random() % 62);
        int i = y * DISC_SIZE + x;
        if (s_cover[i] || s_glow[i] > 12) continue;  // keep clear of the digits and their halo
        s.x = x;
        s.y = y;
        s.start = now + esp_random() % 700;
        s.life = 900 + esp_random() % 1500;
        s.hue = (uint8_t)(esp_random() & 255);
        s.plus = (esp_random() % 3) == 0;
        s.alive = true;
        return;
    }
    s.alive = false;  // nowhere free: try again shortly
    s.start = now + 500;
    s.life = 1;
}

static void star_plot(uint8_t *out, int x, int y, const uint8_t *rgb, float a) {
    if (x < 0 || y < 0 || x >= DISC_SIZE || y >= DISC_SIZE) return;
    int i = y * DISC_SIZE + x;
    if (s_cover[i] > 8) return;  // never paint over a digit
    for (int k = 0; k < 3; k++) {
        int v = out[i * 3 + k] + (int)(rgb[k] * a);
        out[i * 3 + k] = (uint8_t)(v > 255 ? 255 : v);
    }
}

static void draw_stars(uint32_t t_ms, uint8_t phase, uint8_t *out) {
    if (!s_stars_ready) {
        for (auto &s : s_stars) spawn_star(s, t_ms);
        s_stars_ready = true;
    }
    for (auto &s : s_stars) {
        if (t_ms >= s.start + s.life) {
            spawn_star(s, t_ms);
            continue;
        }
        if (t_ms < s.start) continue;
        float u = (float)(t_ms - s.start) / (float)s.life;
        float a = sinf((float)M_PI * u);
        a *= a;  // smooth fade in and out
        const uint8_t *c = s_pal[(uint8_t)(s.hue + phase)];
        uint8_t rgb[3];
        for (int k = 0; k < 3; k++) rgb[k] = (uint8_t)(c[k] + ((255 - c[k]) * 45) / 100);
        star_plot(out, s.x, s.y, rgb, a);
        if (s.plus) {
            star_plot(out, s.x - 1, s.y, rgb, a * 0.4f);
            star_plot(out, s.x + 1, s.y, rgb, a * 0.4f);
            star_plot(out, s.x, s.y - 1, rgb, a * 0.4f);
            star_plot(out, s.x, s.y + 1, rgb, a * 0.4f);
        }
    }
}

void screen_counter_render(uint32_t t_ms, int days, uint8_t scale, uint8_t *out) {
    if (!s_pal_ready) build_palette();
    if (days != s_counter_days) {
        build_counter_image(days);
        s_counter_days = days;
    }
    const uint8_t phase = (uint8_t)(((uint64_t)t_ms * 256 / GRADIENT_PERIOD_MS) & 255);
    for (int i = 0; i < DISC_SIZE * DISC_SIZE; i++) {
        uint8_t *o = &out[i * 3];
        const uint8_t cv = s_cover[i], gl = s_glow[i];
        if (!cv && !gl) {
            o[0] = o[1] = o[2] = 0;
            continue;
        }
        const uint8_t *c = s_pal[(uint8_t)(s_pos[i] + phase)];
        // Stroke: palette color lifted a little toward white so the digits stay crisp; halo: dimmer palette color
        for (int k = 0; k < 3; k++) {
            int stroke = c[k] + ((255 - c[k]) * 22) / 100;
            int halo = (c[k] * gl) / 255 * 45 / 100;
            int v = (stroke * cv) / 255 + (halo * (255 - cv)) / 255;
            o[k] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    draw_stars(t_ms, phase, out);
    apply_scale(out, scale);
}

void screen_update_render(uint32_t t_ms, int percent, uint8_t scale, uint8_t *out) {
    memset(out, 0, DISC_SIZE * DISC_SIZE * 3);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    text_centered(out, 17, "UPDATING", 235, 235, 235);

    // Progress bar 52 x 7 px
    const int bx = 6, by = 28, bw = 52, bh = 7;
    for (int x = bx; x < bx + bw; x++) {
        put(out, x, by, 90, 90, 90);
        put(out, x, by + bh - 1, 90, 90, 90);
    }
    for (int y = by; y < by + bh; y++) {
        put(out, bx, y, 90, 90, 90);
        put(out, bx + bw - 1, y, 90, 90, 90);
    }
    int fill = (bw - 4) * percent / 100;
    for (int y = by + 2; y < by + bh - 2; y++)
        for (int x = 0; x < fill; x++) put(out, bx + 2 + x, y, 30, 215, 96);

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", percent);
    text_centered(out, 41, pct, 30, 215, 96);
    // A small moving dot shows the board is alive even when the percentage is stuck
    int dot = (t_ms / 250) % 4;
    for (int i = 0; i < 4; i++) put(out, 28 + i * 3, 52, i == dot ? 235 : 50, i == dot ? 235 : 50, i == dot ? 235 : 50);
    apply_scale(out, scale);
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
