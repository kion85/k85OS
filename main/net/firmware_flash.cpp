#include "firmware_flash.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "cJSON.h"

#include <cstdio>
#include <cstring>

static const char *TAG = "k85_fwflash";

#define K85_FW_OWNER "kion85"
#define K85_FW_REPO  "k85OS"

static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t *s_ota_partition = nullptr;
static bool s_stream_active = false;

const char *k85_fwflash_free_slot_label(void) {
    const esp_partition_t *p = esp_ota_get_next_update_partition(nullptr);
    return p ? p->label : "?";
}

bool k85_fwflash_stream_begin(void) {
    if (s_stream_active) return false;

    s_ota_partition = esp_ota_get_next_update_partition(nullptr);
    if (!s_ota_partition) {
        ESP_LOGE(TAG, "no free OTA partition found");
        return false;
    }

    esp_err_t err = esp_ota_begin(s_ota_partition, OTA_SIZE_UNKNOWN, &s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %d", (int)err);
        return false;
    }

    s_stream_active = true;
    return true;
}

bool k85_fwflash_stream_write(const uint8_t *data, size_t len) {
    if (!s_stream_active) return false;
    esp_err_t err = esp_ota_write(s_ota_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %d", (int)err);
        return false;
    }
    return true;
}

bool k85_fwflash_stream_end(void) {
    if (!s_stream_active) return false;
    s_stream_active = false;

    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %d", (int)err);
        return false;
    }
    // Намеренно НЕ вызываем esp_ota_set_boot_partition — слот только записан, не активирован
    ESP_LOGI(TAG, "Firmware written to free slot: %s", s_ota_partition ? s_ota_partition->label : "?");
    return true;
}

void k85_fwflash_stream_abort(void) {
    if (!s_stream_active) return;
    s_stream_active = false;
    esp_ota_abort(s_ota_handle);
}

// ---------- Список релизов k85OS ----------
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

bool k85_fwflash_list_available(char out_names[][64], char out_urls[][256], int max, int *out_count) {
    *out_count = 0;

    char url[160];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases/latest", K85_FW_OWNER, K85_FW_REPO);

    static char json_buf[4096];
    HttpBuf buf = { json_buf, 0, sizeof(json_buf) };
    json_buf[0] = 0;

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
    if (err != ESP_OK || status != 200) return false;

    cJSON *root = cJSON_Parse(json_buf);
    if (!root) return false;

    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (!cJSON_IsArray(assets)) { cJSON_Delete(root); return false; }

    int n = cJSON_GetArraySize(assets);
    int found = 0;
    for (int i = 0; i < n && found < max; i++) {
        cJSON *asset = cJSON_GetArrayItem(assets, i);
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        cJSON *dl_url = cJSON_GetObjectItem(asset, "browser_download_url");
        if (!cJSON_IsString(name) || !cJSON_IsString(dl_url)) continue;

        size_t nlen = strlen(name->valuestring);
        if (nlen <= 4 || strcasecmp(name->valuestring + nlen - 4, ".bin") != 0) continue;

        snprintf(out_names[found], 64, "%.63s", name->valuestring);
        snprintf(out_urls[found], 256, "%.255s", dl_url->valuestring);
        found++;
    }
    cJSON_Delete(root);
    *out_count = found;
    return found > 0;
}

bool k85_fwflash_from_url(const char *url, k85_fwflash_progress_cb cb) {
    if (!k85_fwflash_stream_begin()) return false;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { k85_fwflash_stream_abort(); return false; }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        k85_fwflash_stream_abort();
        return false;
    }
    int content_len = esp_http_client_fetch_headers(client);

    uint8_t buf[1024];
    int total_read = 0;
    int r;
    bool ok = true;
    while ((r = esp_http_client_read(client, (char *)buf, sizeof(buf))) > 0) {
        if (!k85_fwflash_stream_write(buf, r)) { ok = false; break; }
        total_read += r;
        if (cb && content_len > 0) cb((total_read * 100) / content_len);
    }
    if (r < 0) ok = false;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok) {
        k85_fwflash_stream_abort();
        return false;
    }
    return k85_fwflash_stream_end();
}
