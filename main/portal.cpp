#include "portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "app_config.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "portal";
static bool s_active = false;

// ---------------------------------------------------------------- HTML

static const char kPageHead[] = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LED Disc setup</title>
<style>
:root{--bg:#f4f1ea;--card:#fff;--ink:#1d1b16;--mute:#6f6a5e;--line:#ddd7c8;--acc:#1db954;--accd:#12803a}
@media(prefers-color-scheme:dark){:root{--bg:#121410;--card:#1c1f1a;--ink:#f1efe8;--mute:#9b9a8e;--line:#34382f}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,sans-serif}
main{max-width:30rem;margin:0 auto;padding:20px 16px 40px}
h1{font-size:1.5rem;margin:.2rem 0 .3rem}p.lead{color:var(--mute);margin:0 0 1.2rem}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:14px}
label{display:block;font-weight:600;margin:0 0 6px}small{color:var(--mute);display:block;margin-top:6px}
input,textarea{width:100%;font:inherit;color:var(--ink);background:transparent;border:1px solid var(--line);border-radius:10px;padding:11px 12px}
textarea{min-height:5.5rem;font-family:ui-monospace,monospace;font-size:.85rem;word-break:break-all}
.field{margin-bottom:14px}.field:last-child{margin-bottom:0}
.nets{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
.nets button{font:inherit;font-size:.9rem;padding:6px 11px;border-radius:999px;border:1px solid var(--line);background:transparent;color:var(--ink)}
button.go{width:100%;font:inherit;font-weight:700;padding:14px;border:0;border-radius:12px;background:var(--acc);color:#04150a}
button.go:active{background:var(--accd)}a{color:var(--accd)}
a.btn{display:block;text-align:center;text-decoration:none;font-weight:700;padding:13px;border-radius:12px;background:var(--acc);color:#04150a;margin-bottom:6px}
</style></head><body><main>
<h1>LED Disc setup</h1>
<p class="lead">Tell the disc which Wi-Fi to use and which Spotify account to show.</p>
<form method="post" action="/save">
<div class="card">
<div class="field"><label for="ssid">Wi-Fi name</label>
<input id="ssid" name="ssid" autocomplete="off" autocapitalize="none" required value="@@SSID@@">
<div class="nets" id="nets"></div><small id="scanmsg">Looking for networks...</small></div>
<div class="field"><label for="pass">Wi-Fi password</label>
<input id="pass" name="pass" type="password" autocomplete="off"><small>Leave empty for an open network. Use a 2.4 GHz network.</small></div>
</div>
<div class="card">
<div class="field"><label for="token">Spotify code</label>
@@HELPER@@
<textarea id="token" name="token" autocomplete="off" autocapitalize="none" spellcheck="false" required></textarea>
<small>Paste the code from the Spotify page here.</small></div>
</div>
<button class="go" type="submit">Save and connect</button>
</form>
<script>
fetch('/scan').then(function(r){return r.json()}).then(function(l){
var n=document.getElementById('nets');document.getElementById('scanmsg').textContent=l.length?'Tap a network to select it.':'No networks found - type the name.';
l.forEach(function(s){var b=document.createElement('button');b.type='button';b.textContent=s;b.onclick=function(){document.getElementById('ssid').value=s;document.getElementById('pass').focus()};n.appendChild(b)})
}).catch(function(){document.getElementById('scanmsg').textContent='Type the network name.'});
</script></main></body></html>)HTML";

static const char kSavedPage[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Saved</title>
<style>body{font:18px/1.5 system-ui,sans-serif;max-width:28rem;margin:0 auto;padding:40px 20px;text-align:center}</style></head>
<body><h1>Saved</h1><p>The disc is restarting and joining your Wi-Fi. This hotspot will disappear.</p>
<p>You can reconnect your phone to your normal Wi-Fi.</p></body></html>)HTML";

static const char kErrorPage[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Check the form</title>
<style>body{font:18px/1.5 system-ui,sans-serif;max-width:28rem;margin:0 auto;padding:40px 20px;text-align:center}</style></head>
<body><h1>Missing details</h1><p>Please enter the Wi-Fi name and paste the Spotify token, then try again.</p>
<p><a href="/">Back</a></p></body></html>)HTML";

// ---------------------------------------------------------------- Helpers

static void html_escape(const char *s, std::string &out) {
    for (; *s; s++) {
        switch (*s) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += *s;
        }
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Extracts and URL-decodes `key` from an application/x-www-form-urlencoded body.
static bool form_get(const char *body, const char *key, char *out, size_t outlen) {
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t o = 0;
            while (*v && *v != '&' && o + 1 < outlen) {
                if (*v == '+') {
                    out[o++] = ' ';
                    v++;
                } else if (*v == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    out[o++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 3;
                } else {
                    out[o++] = *v++;
                }
            }
            out[o] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    out[0] = 0;
    return false;
}

// Trims leading/trailing whitespace and removes embedded whitespace/newlines (tokens have none).
static void strip_spaces(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r != ' ' && *r != '\n' && *r != '\r' && *r != '\t') *w++ = *r;
    }
    *w = 0;
}

static void trim_ends(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    char *p = s;
    while (*p == ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

// ---------------------------------------------------------------- HTTP handlers

static esp_err_t root_get(httpd_req_t *req) {
    AppConfig cfg;
    config_load(&cfg);
    std::string page(kPageHead);

    std::string ssid;
    html_escape(cfg.ssid, ssid);
    size_t pos = page.find("@@SSID@@");
    if (pos != std::string::npos) page.replace(pos, 8, ssid);

    std::string helper;
    if (SETUP_HELPER_URL[0]) {
        helper = "<a class=\"btn\" href=\"";
        helper += SETUP_HELPER_URL;
        helper += "\" target=\"_blank\" rel=\"noopener\">Get your Spotify code</a>"
                  "<small style=\"margin:0 0 12px\">No internet on this hotspot? Open that link on your normal "
                  "connection first, copy the code, then come back here.</small>";
    }
    pos = page.find("@@HELPER@@");
    if (pos != std::string::npos) page.replace(pos, 10, helper);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page.c_str(), page.size());
}

static esp_err_t scan_get(httpd_req_t *req) {
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    esp_wifi_scan_start(&sc, true);
    uint16_t n = 20;
    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(n, sizeof(wifi_ap_record_t));
    std::string json = "[";
    if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        bool first = true;
        for (int i = 0; i < n; i++) {
            const char *s = (const char *)recs[i].ssid;
            if (!s[0]) continue;
            bool dup = false;
            for (int j = 0; j < i; j++) {
                if (strcmp((const char *)recs[j].ssid, s) == 0) dup = true;
            }
            if (dup) continue;
            if (!first) json += ',';
            first = false;
            json += '"';
            for (; *s; s++) {
                if (*s == '"' || *s == '\\') {
                    json += '\\';
                    json += *s;
                } else if ((unsigned char)*s >= 0x20) {
                    json += *s;
                }
            }
            json += '"';
        }
    }
    json += "]";
    free(recs);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.size());
}

static void restart_cb(void *) { esp_restart(); }

static esp_err_t save_post(httpd_req_t *req) {
    if (req->content_len == 0 || req->content_len > 3000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }
    char *body = (char *)calloc(1, req->content_len + 1);
    if (!body) return ESP_FAIL;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }

    AppConfig cfg = {};
    form_get(body, "ssid", cfg.ssid, sizeof(cfg.ssid));
    form_get(body, "pass", cfg.password, sizeof(cfg.password));
    form_get(body, "token", cfg.refresh_token, sizeof(cfg.refresh_token));
    free(body);
    trim_ends(cfg.ssid);
    strip_spaces(cfg.refresh_token);

    if (!cfg.ssid[0] || strlen(cfg.refresh_token) < 20) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, kErrorPage, sizeof(kErrorPage) - 1);
    }
    if (!config_save(&cfg)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not save");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Saved Wi-Fi \"%s\" and Spotify token (%d chars); restarting", cfg.ssid, (int)strlen(cfg.refresh_token));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, kSavedPage, sizeof(kSavedPage) - 1);

    esp_timer_handle_t t;
    esp_timer_create_args_t a = {};
    a.callback = restart_cb;
    a.name = "restart";
    esp_timer_create(&a, &t);
    esp_timer_start_once(t, 2000000);
    return ESP_OK;
}

// Anything else (captive-portal probes like /generate_204, /hotspot-detect.html...) -> the form.
static esp_err_t catchall(httpd_req_t *req, httpd_err_code_t) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static void start_http(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get, .user_ctx = nullptr};
    httpd_uri_t scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get, .user_ctx = nullptr};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post, .user_ctx = nullptr};
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &scan);
    httpd_register_uri_handler(srv, &save);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, catchall);
}

// ---------------------------------------------------------------- Captive DNS (answers everything with 192.168.4.1)

static void dns_task(void *) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        vTaskDelete(nullptr);
        return;
    }
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        vTaskDelete(nullptr);
        return;
    }
    uint8_t in[512], out[600];
    for (;;) {
        sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, in, sizeof(in), 0, (sockaddr *)&from, &fl);
        if (n < 17) continue;
        int i = 12;
        while (i < n && in[i] != 0) i += in[i] + 1;
        i += 1 + 4;  // terminating zero + QTYPE + QCLASS
        if (i > n) continue;
        uint16_t qtype = (in[i - 4] << 8) | in[i - 3];

        memcpy(out, in, i);
        out[2] = 0x81;  // response, recursion desired
        out[3] = 0x80;  // recursion available, no error
        out[6] = 0;
        out[7] = (qtype == 1) ? 1 : 0;  // answer count
        out[8] = out[9] = out[10] = out[11] = 0;
        int len = i;
        if (qtype == 1) {
            static const uint8_t ans[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
                                          0x00, 0x04, 192,  168,  4,    1};
            memcpy(out + len, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(s, out, len, 0, (sockaddr *)&from, fl);
    }
}

// ---------------------------------------------------------------- Public

void portal_start(void) {
    if (s_active) return;
    s_active = true;
    ESP_LOGW(TAG, "Starting setup hotspot \"%s\"", SETUP_AP_SSID);

    esp_wifi_disconnect();  // stop retrying the station so the AP channel stays stable
    wifi_config_t ap = {};
    strncpy((char *)ap.ap.ssid, SETUP_AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(SETUP_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    start_http();
    xTaskCreatePinnedToCore(dns_task, "dns", 4096, nullptr, 2, nullptr, 1);
}

bool portal_active(void) { return s_active; }
