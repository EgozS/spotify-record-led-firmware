#include "ota.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"

static const char *TAG = "ota";
static volatile bool s_active = false;
static volatile int s_percent = -1;

bool ota_active(void) { return s_active; }
int ota_percent(void) { return s_percent; }

void ota_confirm_boot(void) {
    esp_err_t r = esp_ota_mark_app_valid_cancel_rollback();
    if (r == ESP_OK) ESP_LOGI(TAG, "New firmware confirmed (build %d)", FW_BUILD);
}

// ---------------------------------------------------------------- Manifest

struct Body {
    char data[1024];
    size_t len = 0;
};

static esp_err_t body_evt(esp_http_client_event_t *e) {
    Body *b = (Body *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b->len + e->data_len < sizeof(b->data)) {
        memcpy(b->data + b->len, e->data, e->data_len);
        b->len += e->data_len;
        b->data[b->len] = 0;
    }
    return ESP_OK;
}

// Reads the manifest. Returns true and fills build/url when it was fetched and parsed.
static bool fetch_manifest(int *build, char *url, size_t url_len) {
    Body body;
    esp_http_client_config_t cfg = {};
    cfg.url = OTA_MANIFEST_URL;
    cfg.event_handler = body_evt;
    cfg.user_data = &body;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_err_t err = esp_http_client_perform(c);
    int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    if (status != 200) {
        ESP_LOGW(TAG, "Manifest request failed (HTTP %d)", status);
        return false;
    }
    cJSON *j = cJSON_Parse(body.data);
    cJSON *b = j ? cJSON_GetObjectItem(j, "build") : nullptr;
    cJSON *u = j ? cJSON_GetObjectItem(j, "url") : nullptr;
    bool ok = cJSON_IsNumber(b) && cJSON_IsString(u) && strlen(u->valuestring) < url_len &&
              strncmp(u->valuestring, "https://", 8) == 0;
    if (ok) {
        *build = b->valueint;
        strcpy(url, u->valuestring);
    } else {
        ESP_LOGW(TAG, "Manifest is not valid");
    }
    cJSON_Delete(j);
    return ok;
}

// ---------------------------------------------------------------- Download + install

// Returns true only when the new image is fully written, verified and selected for the next boot.
static bool install(const char *url) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    bool ok = false;
    esp_ota_handle_t handle = 0;
    bool began = false;
    uint8_t *buf = (uint8_t *)malloc(4096);
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);

    do {
        if (!buf || !part) break;
        if (esp_http_client_open(c, 0) != ESP_OK) {
            ESP_LOGW(TAG, "Could not open the firmware URL");
            break;
        }
        int64_t total_len = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            ESP_LOGW(TAG, "Firmware download refused (HTTP %d)", status);
            break;
        }
        if (total_len > (int64_t)part->size) {
            ESP_LOGW(TAG, "Firmware (%lld bytes) does not fit the slot", (long long)total_len);
            break;
        }
        if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) break;
        began = true;

        uint8_t hdr[300];
        size_t hdr_len = 0, total = 0;
        bool hdr_checked = false, failed = false;
        for (;;) {
            int r = esp_http_client_read(c, (char *)buf, 4096);
            if (r < 0) {
                ESP_LOGW(TAG, "Download interrupted");
                failed = true;
                break;
            }
            if (r == 0) {
                if (esp_http_client_is_complete_data_received(c)) break;
                if (errno == ENOTCONN || errno == ECONNRESET) {
                    failed = true;
                    break;
                }
                continue;
            }
            // Make sure this really is our firmware before writing much of it
            if (hdr_len < sizeof(hdr)) {
                size_t take = sizeof(hdr) - hdr_len < (size_t)r ? sizeof(hdr) - hdr_len : (size_t)r;
                memcpy(hdr + hdr_len, buf, take);
                hdr_len += take;
            }
            if (!hdr_checked && hdr_len >= sizeof(hdr)) {
                hdr_checked = true;
                const esp_app_desc_t *d = (const esp_app_desc_t *)(hdr + sizeof(esp_image_header_t) +
                                                                      sizeof(esp_image_segment_header_t));
                const esp_app_desc_t *me = esp_app_get_description();
                if (hdr[0] != ESP_IMAGE_HEADER_MAGIC || strncmp(d->project_name, me->project_name, sizeof(d->project_name)) != 0) {
                    ESP_LOGW(TAG, "Downloaded file is not this project's firmware");
                    failed = true;
                    break;
                }
            }
            if (esp_ota_write(handle, buf, r) != ESP_OK) {
                ESP_LOGW(TAG, "Writing to flash failed");
                failed = true;
                break;
            }
            total += r;
            if (total_len > 0) s_percent = (int)(total * 100 / (size_t)total_len);
        }
        if (failed) break;
        if (total_len > 0 && total != (size_t)total_len) {
            ESP_LOGW(TAG, "Size mismatch: got %u of %lld bytes", (unsigned)total, (long long)total_len);
            break;
        }
        s_percent = 100;
        esp_err_t e = esp_ota_end(handle);  // validates the image (checksum / hash)
        began = false;
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "Image validation failed (%s)", esp_err_to_name(e));
            break;
        }
        if (esp_ota_set_boot_partition(part) != ESP_OK) break;
        ok = true;
    } while (0);

    if (began) esp_ota_abort(handle);
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

// ---------------------------------------------------------------- Task

static void ota_task(void *) {
    // Wait for Wi-Fi, then give everything else a moment to settle
    while (!net_get_state().wifi_connected) vTaskDelay(pdMS_TO_TICKS(2000));
    vTaskDelay(pdMS_TO_TICKS(OTA_FIRST_CHECK_S * 1000));

    for (;;) {
        int wait_s = OTA_CHECK_INTERVAL_S;
        if (net_get_state().wifi_connected && !net_get_state().setup_mode) {
            int build = 0;
            char url[256];
            if (fetch_manifest(&build, url, sizeof(url))) {
                ESP_LOGI(TAG, "Running build %d, latest published build %d", FW_BUILD, build);
                if (build > FW_BUILD) {
                    ESP_LOGW(TAG, "Installing build %d from %s", build, url);
                    s_percent = 0;
                    s_active = true;
                    bool ok = install(url);
                    if (ok) {
                        ESP_LOGW(TAG, "Update installed, restarting");
                        vTaskDelay(pdMS_TO_TICKS(1500));
                        esp_restart();
                    }
                    s_active = false;
                    s_percent = -1;
                    wait_s = OTA_RETRY_AFTER_FAIL_S;
                }
            } else {
                wait_s = OTA_RETRY_AFTER_FAIL_S;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
    }
}

void ota_start(void) { xTaskCreatePinnedToCore(ota_task, "ota", 10240, nullptr, 2, nullptr, 1); }
