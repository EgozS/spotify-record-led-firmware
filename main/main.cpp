#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "hub75.h"

#include "app_config.h"
#include "config.h"
#include "counter.h"
#include "disc.h"
#include "net.h"
#include "ota.h"
#include "portal.h"
#include "screen.h"

// Rotary encoder (relative: reports +/- steps)
#define CLK_PIN GPIO_NUM_45
#define DT_PIN  GPIO_NUM_46

// The board's built-in BOOT button (GPIO 0): click = switch record / counter,
// hold = reopen the setup hotspot. (Never hold it while powering on: that enters download mode.)
#define BTN_PIN GPIO_NUM_0

// One level change per TRANSITIONS_PER_STEP valid quadrature transitions
// (a typical detent = 4 transitions). Tune if the knob feels too slow / fast.
#define TRANSITIONS_PER_STEP  2

// Record speed: 67.5 degrees per second (~11 RPM)
#define SPIN_DEG_PER_SEC 67.5f

// Brightness ladder. The hub75 library clamps set_brightness() to a floor of
// ~7% duty, so to get very dim we first scale the picture's colors down (they
// go through a gamma LUT, so low values are extremely dark), and only then raise
// the panel brightness.
struct Level { uint8_t brightness; uint8_t scale; };
static const int kStage2[] = {1, 2, 3, 4, 6, 8, 11, 16, 23, 32, 45, 64, 91, 128, 181, 255};
static const int kStage1Count = 19;                                   // scale 5..241 @ brightness 1
// Highest usable entry of kStage2. Index 10 = brightness 45. Brighter levels (64, 91, 128...) draw more
// current than the panel's 5V supply/wiring delivers: the voltage sags, blues and greens fade first and
// the picture turns oversaturated and warm (at the extreme, red). Tested on the real panel: 64 is still a
// little saturated, 32 a bit too dim, so 45 with the art color correction (app_config.h). Raise it
// (max 15) only after giving the panel its own beefy 5V supply wired directly to its power terminals.
static const int kMaxStage2Index = 10;
static const int kNumLevels = 1 + kStage1Count + kMaxStage2Index + 1;
// Start one level below the maximum (brightness 32, which looked right on the real panel);
// one click up reaches the maximum (45).
static const int kStartLevel = 1 + kStage1Count + kMaxStage2Index - 1;

static Level level_at(int idx) {
    if (idx <= 0) return {0, 0};                                      // off
    if (idx <= kStage1Count) return {1, (uint8_t)(5 + (250 * (idx - 1)) / 19)};
    return {(uint8_t)kStage2[idx - 1 - kStage1Count], 255};
}

static volatile int32_t knob_accum = 0;   // net valid transitions, written by timer
static uint8_t knob_prev = 0;

// Quadrature transition table, index = (prev << 2) | curr. Invalid (bounce) = 0.
static const int8_t kQuadTable[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static void knob_sample_cb(void *) {
    uint8_t curr = (gpio_get_level(CLK_PIN) << 1) | gpio_get_level(DT_PIN);
    if (curr != knob_prev) {
        knob_accum += kQuadTable[(knob_prev << 2) | curr];
        knob_prev = curr;
    }
}

static uint8_t s_frame[DISC_SIZE * DISC_SIZE * 3];
static uint8_t s_art_tmp[ART_SIZE * ART_SIZE * 3];

extern "C" void app_main(void) {
    // 1. Configure 64x64 HUB75 panel
    Hub75Config config = {};
    config.panel_width = 64;
    config.panel_height = 64;
    config.brightness = 128;
    config.double_buffer = true;   // tear-free animation
    config.output_clock_speed = Hub75ClockSpeed::HZ_10M;  // 20 MHz caused the panel to glitch red

    // HUB75 GPIO pins
    config.pins.r1 = 4;
    config.pins.g1 = 5;
    config.pins.b1 = 6;
    config.pins.r2 = 7;
    config.pins.g2 = 15;
    config.pins.b2 = 16;
    config.pins.a  = 18;
    config.pins.b  = 8;
    config.pins.c  = 3;
    config.pins.d  = 42;
    config.pins.e  = 9;  // Required for 64x64
    config.pins.lat = 40;
    config.pins.oe  = 2;
    config.pins.clk = 41;

    Hub75Driver driver(config);
    if (!driver.begin()) {
        printf("Hub75 driver begin() failed\n");
        return;
    }
    printf("Free heap after panel init: %u (largest block %u)\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // 2. Encoder pins, sampled by a 0.5 ms timer (no interrupts -> no storms)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << CLK_PIN) | (1ULL << DT_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    knob_prev = (gpio_get_level(CLK_PIN) << 1) | gpio_get_level(DT_PIN);

    esp_timer_handle_t timer;
    esp_timer_create_args_t targs = {};
    targs.callback = knob_sample_cb;
    targs.name = "knob";
    esp_timer_create(&targs, &timer);
    esp_timer_start_periodic(timer, 500);  // microseconds

    gpio_config_t btn_conf = {};
    btn_conf.pin_bit_mask = 1ULL << BTN_PIN;
    btn_conf.mode = GPIO_MODE_INPUT;
    btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&btn_conf);

    AppConfig cfg;
    config_load(&cfg);

    // 3. Disc renderer + network
    disc_init();
    vTaskPrioritySet(nullptr, 10);  // render loop outranks the network task
    printf("Firmware build %d\n", FW_BUILD);
    net_start();
    ota_start();
    bool boot_confirmed = false;

    int level = kStartLevel;
    Level lv = level_at(level);
    driver.set_brightness(lv.brightness);

    float angle = 0.0f;
    uint32_t art_version = 0;
    bool art_shown = false;
    int64_t last_us = esp_timer_get_time();
    int64_t last_report_us = last_us;

    // 4. Main loop: knob -> brightness level, network state -> spinning disc
    int64_t win_max_frame = 0, win_max_state = 0, win_max_render = 0, win_max_draw = 0, win_max_flip = 0;

    // View selection: the record while music plays, the counter otherwise (after a while);
    // a BOOT-button click overrides this until playback starts or stops.
    enum Override { OV_AUTO, OV_COUNTER, OV_RECORD };
    Override override_view = OV_AUTO;
    bool prev_playing = false;
    int64_t idle_since_us = esp_timer_get_time();
    bool btn_down = false, long_fired = false;
    int64_t btn_down_us = 0;
    int days = -1;
    int64_t days_check_us = 0;

    while (1) {
        int64_t t_start = esp_timer_get_time();
        // Knob
        int32_t acc = knob_accum;
        int steps = acc / TRANSITIONS_PER_STEP;
        if (steps != 0) {
            knob_accum -= steps * TRANSITIONS_PER_STEP;
            int new_level = level + steps;
            if (new_level < 0) new_level = 0;
            if (new_level > kNumLevels - 1) new_level = kNumLevels - 1;
            if (new_level != level) {
                level = new_level;
                Level nl = level_at(level);
                // Only touch the panel's brightness register when it really changes:
                // the library rewrites live DMA data on every call.
                if (nl.brightness != lv.brightness) driver.set_brightness(nl.brightness);
                lv = nl;
                printf("Level %d/%d  brightness=%d scale=%d\n", level, kNumLevels - 1, lv.brightness, lv.scale);
            }
        }

        // Network state
        NetState st = net_get_state();
        if (st.has_track && st.has_art) {
            if (!art_shown || st.art_version != art_version) {
                art_version = net_copy_art(s_art_tmp);
                disc_set_art(s_art_tmp);
                art_shown = true;
            }
        } else if (art_shown) {
            disc_set_art(nullptr);
            art_shown = false;
        }

        // Spin only while something is playing
        int64_t now_us = esp_timer_get_time();
        float dt = (now_us - last_us) / 1e6f;
        last_us = now_us;
        if (st.has_track && st.playing) {
            angle += dt * SPIN_DEG_PER_SEC * (float)M_PI / 180.0f;
            if (angle > 2.0f * (float)M_PI) angle -= 2.0f * (float)M_PI;
        }

        int64_t t_state = esp_timer_get_time();
        // Playback state -> idle timer; any play/stop change clears a manual override
        bool playing_now = st.has_track && st.playing;
        if (playing_now != prev_playing) {
            prev_playing = playing_now;
            override_view = OV_AUTO;
            if (!playing_now) idle_since_us = now_us;
        }
        bool auto_counter = !playing_now && (now_us - idle_since_us) >= (int64_t)AUTO_COUNTER_AFTER_S * 1000000;
        bool show_counter = override_view == OV_COUNTER || (override_view == OV_AUTO && auto_counter);

        // BOOT button: click toggles the view, a long press opens the setup hotspot
        bool pressed = gpio_get_level(BTN_PIN) == 0;
        if (pressed && !btn_down) {
            btn_down = true;
            long_fired = false;
            btn_down_us = now_us;
        } else if (pressed && btn_down && !long_fired && now_us - btn_down_us > (int64_t)LONG_PRESS_MS * 1000) {
            long_fired = true;
            printf("BOOT button held: opening setup hotspot\n");
            portal_start();
        } else if (!pressed && btn_down) {
            btn_down = false;
            if (!long_fired && now_us - btn_down_us > 40000) {
                override_view = show_counter ? OV_RECORD : OV_COUNTER;
                show_counter = override_view == OV_COUNTER;
                printf("BOOT button click: showing %s\n", show_counter ? "counter" : "record");
            }
        }

        if (now_us - days_check_us > 1000000) {  // recompute once a second
            days_check_us = now_us;
            days = counter_days(TOGETHER_SINCE, cfg.tz_min);
        }

        // A freshly installed update that has run healthy for 45 s is kept; otherwise the bootloader
        // reverts to the previous version on the next restart.
        if (!boot_confirmed && now_us > 45000000) {
            boot_confirmed = true;
            ota_confirm_boot();
        }

        if (st.ota_active) {
            screen_update_render((uint32_t)(now_us / 1000), st.ota_percent, lv.scale, s_frame);
        } else if (st.setup_mode) {
            screen_setup_render((uint32_t)(now_us / 1000), lv.scale, s_frame);
        } else if (show_counter) {
            screen_counter_render((uint32_t)(now_us / 1000), days, lv.scale, s_frame);
        } else {
            disc_render(angle, lv.scale, s_frame);
        }
        int64_t t_render = esp_timer_get_time();
        driver.draw_pixels(0, 0, DISC_SIZE, DISC_SIZE, s_frame, Hub75PixelFormat::RGB888);
        int64_t t_draw = esp_timer_get_time();
        driver.flip_buffer();
        int64_t t_flip = esp_timer_get_time();

        // Frame timing diagnostics (max over a 10 s window)
        win_max_state = std::max(win_max_state, t_state - t_start);
        win_max_render = std::max(win_max_render, t_render - t_state);
        win_max_draw = std::max(win_max_draw, t_draw - t_render);
        win_max_flip = std::max(win_max_flip, t_flip - t_draw);
        win_max_frame = std::max(win_max_frame, t_flip - t_start);
        if (t_flip - t_start > 100000) {
            printf("[slow frame] total=%lldms state=%lld render=%lld draw=%lld flip=%lld (ms)\n",
                   (t_flip - t_start) / 1000, (t_state - t_start) / 1000, (t_render - t_state) / 1000,
                   (t_draw - t_render) / 1000, (t_flip - t_draw) / 1000);
        }

        if (now_us - last_report_us > 10000000) {
            last_report_us = now_us;
            printf("[status] build=%d wifi=%d track=%d playing=%d art=%d days=%d heap=%u internal=%u | max ms: frame=%lld state=%lld render=%lld draw=%lld flip=%lld\n",
                   FW_BUILD, st.wifi_connected, st.has_track, st.playing, st.has_art, days,
                   (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   win_max_frame / 1000, win_max_state / 1000, win_max_render / 1000, win_max_draw / 1000,
                   win_max_flip / 1000);
            win_max_frame = win_max_state = win_max_render = win_max_draw = win_max_flip = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
