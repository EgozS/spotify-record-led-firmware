#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <algorithm>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "cJSON.h"
#include "jpeg_decoder.h"

#include "app_config.h"
#include "config.h"
#include "ota.h"
#include "portal.h"

static const char *TAG = "net";

#define WIFI_CONNECTED_BIT BIT0
#define POLL_INTERVAL_MS 2500

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_mutex;
static NetState s_state;
static uint8_t s_art[ART_SIZE * ART_SIZE * 3];
static AppConfig s_cfg;
static bool s_ever_connected = false;

// ---------------------------------------------------------------- Wi-Fi

static void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_cfg.ssid[0] && !portal_active()) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.wifi_connected = false;
        xSemaphoreGive(s_mutex);
        if (portal_active()) return;  // setup hotspot is up: keep the radio channel stable
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&ev->ip_info.ip));
        s_ever_connected = true;
        static bool sntp_started = false;
        if (!sntp_started) {  // real clock for the days-together counter
            sntp_started = true;
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sc);
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.wifi_connected = true;
        xSemaphoreGive(s_mutex);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr));

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, s_cfg.ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_cfg.password, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = s_cfg.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ---------------------------------------------------------------- HTTP

struct Buf {
    uint8_t *data = nullptr;
    size_t len = 0;
    size_t cap = 0;
    size_t max = 0;
    bool overflow = false;
    int retry_after = 0;
};

static esp_err_t http_evt(esp_http_client_event_t *e) {
    Buf *b = (Buf *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(e->header_key, "Retry-After") == 0) b->retry_after = atoi(e->header_value);
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
        if (b->len + e->data_len > b->max) {
            b->overflow = true;
            return ESP_OK;
        }
        if (b->len + e->data_len + 1 > b->cap) {
            size_t nc = std::max(b->cap * 2, b->len + e->data_len + 1 + 1024);
            uint8_t *nd = (uint8_t *)realloc(b->data, nc);
            if (!nd) {
                b->overflow = true;
                return ESP_OK;
            }
            b->data = nd;
            b->cap = nc;
        }
        memcpy(b->data + b->len, e->data, e->data_len);
        b->len += e->data_len;
        b->data[b->len] = 0;
    }
    return ESP_OK;
}

// Returns the HTTP status code, or -1 on transport error.
static int http_request(const char *url, const char *bearer, const char *post_body, Buf *b) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_evt;
    cfg.user_data = b;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1536;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (post_body) {
        esp_http_client_set_method(c, HTTP_METHOD_POST);
        esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(c, post_body, strlen(post_body));
    }
    if (bearer) {
        char hdr[700];
        snprintf(hdr, sizeof(hdr), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", hdr);
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(c) : -1;
    if (err != ESP_OK) ESP_LOGW(TAG, "HTTP error: %s", esp_err_to_name(err));
    esp_http_client_cleanup(c);
    return status;
}

// ---------------------------------------------------------------- Spotify auth

static char s_token[700];
static int64_t s_token_expiry_us = 0;

static bool refresh_token(void) {
    if (!s_cfg.refresh_token[0]) {
        portal_start();  // no Spotify token saved yet
        return false;
    }
    char body[800];
    snprintf(body, sizeof(body), "grant_type=refresh_token&refresh_token=%s&client_id=%s", s_cfg.refresh_token,
             SPOTIFY_CLIENT_ID);
    Buf b;
    b.max = 8192;
    int st = http_request("https://accounts.spotify.com/api/token", nullptr, body, &b);
    bool ok = false;
    if (st == 200 && b.data) {
        cJSON *r = cJSON_Parse((const char *)b.data);
        cJSON *tok = r ? cJSON_GetObjectItem(r, "access_token") : nullptr;
        cJSON *exp = r ? cJSON_GetObjectItem(r, "expires_in") : nullptr;
        if (cJSON_IsString(tok) && strlen(tok->valuestring) < sizeof(s_token)) {
            strcpy(s_token, tok->valuestring);
            int secs = cJSON_IsNumber(exp) ? exp->valueint : 3600;
            s_token_expiry_us = esp_timer_get_time() + (int64_t)(secs - 60) * 1000000;
            ok = true;
            ESP_LOGI(TAG, "Got Spotify access token (valid %d s)", secs);
        }
        cJSON_Delete(r);
    } else {
        ESP_LOGW(TAG, "Token refresh failed (HTTP %d): %s", st, b.data ? (const char *)b.data : "");
        if (st == 400) portal_start();  // token rejected (revoked / wrong): ask for a new one
    }
    free(b.data);
    return ok;
}

// ---------------------------------------------------------------- Album art

// Downloads + decodes a JPEG into s_art (64x64 RGB888). Returns true on success.
static bool fetch_art(const char *url) {
    Buf b;
    b.max = 96 * 1024;
    int st = http_request(url, nullptr, nullptr, &b);
    if (st != 200 || b.overflow || !b.data) {
        ESP_LOGW(TAG, "Art download failed (HTTP %d, overflow=%d)", st, b.overflow);
        free(b.data);
        return false;
    }

    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata = b.data;
    cfg.indata_size = (uint32_t)b.len;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
    cfg.out_scale = JPEG_IMAGE_SCALE_0;
    esp_jpeg_image_output_t info = {};
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
        ESP_LOGW(TAG, "Art: unsupported JPEG (progressive?)");
        free(b.data);
        return false;
    }
    if (info.width > 200 || info.height > 200 || info.width == 0 || info.height == 0) {
        ESP_LOGW(TAG, "Art: image too large (%dx%d)", info.width, info.height);
        free(b.data);
        return false;
    }
    uint8_t *rgb = (uint8_t *)malloc(info.output_len);
    if (!rgb) {
        free(b.data);
        return false;
    }
    cfg.outbuf = rgb;
    cfg.outbuf_size = (uint32_t)info.output_len;
    bool ok = esp_jpeg_decode(&cfg, &info) == ESP_OK;
    free(b.data);
    if (!ok) {
        ESP_LOGW(TAG, "Art: decode failed");
        free(rgb);
        return false;
    }

    // Resample to 64x64 (box-less nearest; the 64px Spotify image is a straight copy).
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int y = 0; y < ART_SIZE; y++) {
        int sy = y * info.height / ART_SIZE;
        for (int x = 0; x < ART_SIZE; x++) {
            int sx = x * info.width / ART_SIZE;
            memcpy(&s_art[(y * ART_SIZE + x) * 3], &rgb[(sy * info.width + sx) * 3], 3);
        }
    }
    s_state.has_art = true;
    s_state.art_version++;
    xSemaphoreGive(s_mutex);
    free(rgb);
    ESP_LOGI(TAG, "Album art updated (source %dx%d)", info.width, info.height);
    return true;
}

// ---------------------------------------------------------------- Polling

static char s_cur_art_url[300] = "";
static char s_failed_art_url[300] = "";

static void set_playback(bool has_track, bool playing) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.has_track = has_track;
    s_state.playing = playing;
    xSemaphoreGive(s_mutex);
}

// Pick the smallest image that is at least 64px wide (else the largest available).
static const char *pick_image_url(cJSON *images) {
    const char *best = nullptr;
    int best_w = 0;
    cJSON *img;
    cJSON_ArrayForEach(img, images) {
        cJSON *u = cJSON_GetObjectItem(img, "url");
        cJSON *w = cJSON_GetObjectItem(img, "width");
        if (!cJSON_IsString(u)) continue;
        int width = cJSON_IsNumber(w) ? w->valueint : 0;
        bool better;
        if (!best) {
            better = true;
        } else if (best_w >= ART_SIZE) {
            better = width >= ART_SIZE && width < best_w;
        } else {
            better = width > best_w;
        }
        if (better) {
            best = u->valuestring;
            best_w = width;
        }
    }
    return best;
}

// Returns the number of ms to wait before the next poll.
static int poll_once(void) {
    if (esp_timer_get_time() >= s_token_expiry_us) {
        if (!refresh_token()) return 10000;
    }

    Buf b;
    b.max = 48 * 1024;
    int st = http_request("https://api.spotify.com/v1/me/player/currently-playing?additional_types=episode", s_token,
                          nullptr, &b);
    int wait = POLL_INTERVAL_MS;

    if (st == 204) {
        set_playback(false, false);
    } else if (st == 200 && b.data && !b.overflow) {
        cJSON *root = cJSON_Parse((const char *)b.data);
        cJSON *item = root ? cJSON_GetObjectItem(root, "item") : nullptr;
        if (!cJSON_IsObject(item)) {
            set_playback(false, false);  // ad / nothing loaded
        } else {
            bool playing = cJSON_IsTrue(cJSON_GetObjectItem(root, "is_playing"));
            set_playback(true, playing);
            cJSON *album = cJSON_GetObjectItem(item, "album");
            cJSON *images = cJSON_GetObjectItem(cJSON_IsObject(album) ? album : item, "images");
            const char *url = pick_image_url(images);
            if (url && strcmp(url, s_cur_art_url) != 0 && strcmp(url, s_failed_art_url) != 0 &&
                strlen(url) < sizeof(s_cur_art_url)) {
                char tmp[300];
                strcpy(tmp, url);
                cJSON_Delete(root);  // free the big JSON before the TLS download
                root = nullptr;
                free(b.data);
                b.data = nullptr;
                if (fetch_art(tmp)) {
                    strcpy(s_cur_art_url, tmp);
                } else {
                    strcpy(s_failed_art_url, tmp);
                }
            }
        }
        if (root) cJSON_Delete(root);
    } else if (st == 401) {
        s_token_expiry_us = 0;  // force refresh next time
    } else if (st == 429) {
        wait = std::max(b.retry_after, 5) * 1000;
        ESP_LOGW(TAG, "Rate limited, waiting %d ms", wait);
    } else {
        ESP_LOGW(TAG, "Now-playing request failed (HTTP %d)", st);
        wait = 5000;
    }
    free(b.data);
    return wait;
}

static void net_task(void *) {
    bool have_ssid = config_load(&s_cfg);
    wifi_start();
    if (!have_ssid) portal_start();  // brand new unit: nothing to connect to yet

    int down_ms = 0;
    for (;;) {
        bool up = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000)) &
                  WIFI_CONNECTED_BIT;
        if (!up) {
            // Never managed to connect since boot (new place / wrong password): open the setup hotspot.
            down_ms += 1000;
            if (!s_ever_connected && down_ms >= WIFI_CONNECT_TIMEOUT_MS && !portal_active()) portal_start();
            continue;
        }
        down_ms = 0;
        int wait = poll_once();
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

// ---------------------------------------------------------------- Public API

void net_start(void) {
    s_mutex = xSemaphoreCreateMutex();
    s_wifi_events = xEventGroupCreate();
    // Pinned to core 1 at low priority so TLS handshakes can never starve the
    // render loop (which runs at higher priority on core 0).
    xTaskCreatePinnedToCore(net_task, "net", 12288, nullptr, 2, nullptr, 1);
}

NetState net_get_state(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    NetState s = s_state;
    xSemaphoreGive(s_mutex);
    s.setup_mode = portal_active();
    s.ota_active = ota_active();
    s.ota_percent = ota_percent();
    return s;
}

uint32_t net_copy_art(uint8_t *dst) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(dst, s_art, sizeof(s_art));
    uint32_t v = s_state.art_version;
    xSemaphoreGive(s_mutex);
    return v;
}
