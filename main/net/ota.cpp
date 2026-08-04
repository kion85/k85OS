#include "ota.h"
#include "../core/version.h"
#include "../core/notifications.h"
#include "wifi.h"
#include "config.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>

static const char *TAG = "k85_ota";

// ---------- HTTP GET в буфер (для GitHub API JSON) ----------
struct HttpBuf {
    char *data;
    size_t len;
    size_t cap;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    HttpBuf *buf = (HttpBuf *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
        if (buf->len + evt->data_len < buf->cap) {
            memcpy(buf->data + buf->len, evt->data, evt->data_len);
            buf->len += evt->data_len;
            buf->data[buf->len] = 0;
        }
    }
    return ESP_OK;
}

static bool fetch_latest_release_json(char *out, size_t out_cap) {
    char url[160];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases/latest",
             K85_GITHUB_OWNER, K85_GITHUB_REPO);

    HttpBuf buf = { out, 0, out_cap };
    out[0] = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_event_handler;
    cfg.user_data = &buf;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 8000;
    cfg.user_agent = "k85OS-device";

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GitHub API request failed: err=%d status=%d", err, status);
        return false;
    }
    return true;
}

// Сравнение версий вида "4.2" > "4.1"
static bool version_is_newer(const char *remote, const char *local) {
    int r_maj = 0, r_min = 0, l_maj = 0, l_min = 0;
    sscanf(remote, "%d.%d", &r_maj, &r_min);
    sscanf(local, "%d.%d", &l_maj, &l_min);
    if (r_maj != l_maj) return r_maj > l_maj;
    return r_min > l_min;
}

bool k85_ota_check_update(char *out_version, size_t ver_size, char *out_url, size_t url_size) {
    if (g_config.ota_locked) return false;
    static char json_buf[4096];
    if (!fetch_latest_release_json(json_buf, sizeof(json_buf))) return false;

    cJSON *root = cJSON_Parse(json_buf);
    if (!root) return false;

    bool found = false;
    cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    cJSON *assets = cJSON_GetObjectItem(root, "assets");

    if (cJSON_IsString(tag) && cJSON_IsArray(assets)) {
        const char *tag_str = tag->valuestring;
        if (tag_str[0] == 'v' || tag_str[0] == 'V') tag_str++; // убрать 'v' из "v4.2"

        if (version_is_newer(tag_str, K85_FW_VERSION)) {
            int n = cJSON_GetArraySize(assets);
            for (int i = 0; i < n; i++) {
                cJSON *asset = cJSON_GetArrayItem(assets, i);
                cJSON *name = cJSON_GetObjectItem(asset, "name");
                cJSON *dl_url = cJSON_GetObjectItem(asset, "browser_download_url");
                if (cJSON_IsString(name) && cJSON_IsString(dl_url)) {
                    size_t nlen = strlen(name->valuestring);
                    // ищем именно .bin (не bootloader/partition-table, если их тоже прикладываешь)
                    if (nlen > 4 && strcmp(name->valuestring + nlen - 4, ".bin") == 0) {
                        snprintf(out_version, ver_size, "%s", tag_str);
                        snprintf(out_url, url_size, "%s", dl_url->valuestring);
                        found = true;
                        break;
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
    return found;
}

// ---------- Прошивка ----------
bool k85_ota_perform_update(const char *url, k85_ota_progress_cb progress_cb) {
    if (g_config.ota_locked) return false;
    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.timeout_ms = 15000;
    http_cfg.keep_alive_enable = true;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %d", err);
        return false;
    }

    int image_size = esp_https_ota_get_image_size(handle);

    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        if (progress_cb && image_size > 0) {
            int read_so_far = esp_https_ota_get_image_len_read(handle);
            progress_cb((read_so_far * 100) / image_size);
        }
    }

    bool ota_finished_ok = false;
    if (err == ESP_OK && esp_https_ota_is_complete_data_received(handle)) {
        if (esp_https_ota_finish(handle) == ESP_OK) {
            ota_finished_ok = true;
        }
    } else {
        esp_https_ota_abort(handle);
    }

    if (!ota_finished_ok) {
        ESP_LOGE(TAG, "OTA failed");
        return false;
    }

    ESP_LOGI(TAG, "OTA success, restarting...");
    if (progress_cb) progress_cb(100);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return true; // не достигнется
}

// ---------- Фоновая проверка ----------
static uint32_t s_interval_ms = 0;

static void ota_check_task(void *arg) {
    char ver[16];
    char url[256];

    while (true) {
        if (!k85_wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(s_interval_ms));
            continue;
        }
        if (k85_ota_check_update(ver, sizeof(ver), url, sizeof(url))) {
            k85_notify("Firmware update available: v%s", ver);
            ESP_LOGI(TAG, "Update found: v%s -> %s", ver, url);
        } else {
            ESP_LOGI(TAG, "No update available (current: v%s)", K85_FW_VERSION);
        }
        vTaskDelay(pdMS_TO_TICKS(s_interval_ms));
    }
}

void k85_ota_start_background_check(uint32_t interval_ms) {
    s_interval_ms = interval_ms;
    xTaskCreate(ota_check_task, "ota_check", 8192, nullptr, 3, nullptr);
}



