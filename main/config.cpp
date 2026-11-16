#include "config.h"

#include <string.h>

#include "app_config.h"
#include "nvs.h"
#include "nvs_flash.h"

static void nvs_ready(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

static bool get_str(nvs_handle_t h, const char *key, char *dst, size_t n) {
    size_t len = n;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        dst[0] = 0;
        return false;
    }
    return true;
}

bool config_load(AppConfig *c) {
    memset(c, 0, sizeof(*c));
    nvs_ready();
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ssid", c->ssid, sizeof(c->ssid));
        get_str(h, "pass", c->password, sizeof(c->password));
        get_str(h, "token", c->refresh_token, sizeof(c->refresh_token));
        nvs_close(h);
    }
#ifdef WIFI_SSID
    if (!c->ssid[0]) {
        strncpy(c->ssid, WIFI_SSID, sizeof(c->ssid) - 1);
        strncpy(c->password, WIFI_PASSWORD, sizeof(c->password) - 1);
    }
#endif
#ifdef SPOTIFY_REFRESH_TOKEN
    if (!c->refresh_token[0]) strncpy(c->refresh_token, SPOTIFY_REFRESH_TOKEN, sizeof(c->refresh_token) - 1);
#endif
    return c->ssid[0] != 0;
}

bool config_save(const AppConfig *c) {
    nvs_ready();
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", c->ssid) == ESP_OK && nvs_set_str(h, "pass", c->password) == ESP_OK &&
              nvs_set_str(h, "token", c->refresh_token) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
