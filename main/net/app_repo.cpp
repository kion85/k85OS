#include "app_repo.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "core/heavy_lock.h"

#include <cstdio>
#include <cstring>

static const char *TAG = "k85_apprepo";

#define K85_APPREPO_OWNER "kion85"
#define K85_APPREPO_REPO  "apps_k85os"

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
        } else {
            ESP_LOGW(TAG, "http response truncated, buffer too small (cap=%u)", (unsigned)buf->cap);
        }
    }
    return ESP_OK;
}

bool k85_apprepo_fetch_uefi_theme_list(char out_names[][64], char out_urls[][256], int max, int *out_count) {
    *out_count = 0;

    K85HeavyLockGuard heavy_lock(15000);
    if (!heavy_lock.held) {
        ESP_LOGW(TAG, "uefi fetch: heavy lock busy, skipping");
        return false;
    }

    char url[160];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases/tags/uefi",
             K85_APPREPO_OWNER, K85_APPREPO_REPO);

    // 4096 байт было мало для ответа GitHub API с 13+ ассетами (JSON легко
    // превышает 6-10KB) — http_event_handler молча отбрасывал "хвост",
    // cJSON_Parse получал битый JSON и возвращал false ("No themes found",
    // хотя релиз реально существовал и был заполнен). Увеличиваем буфер и
    // держим его в PSRAM, а не в internal RAM (по 16KB на статический
    // internal-буфер, которым пользуются раз в сто лет, не разбрасываемся).
    #define K85_APPREPO_JSON_BUF_SIZE 32768
    static char *json_buf = nullptr;
    if (!json_buf) {
        json_buf = (char *)heap_caps_malloc(K85_APPREPO_JSON_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!json_buf) return false;
    }
    HttpBuf buf = { json_buf, 0, K85_APPREPO_JSON_BUF_SIZE };
    json_buf[0] = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_event_handler;
    cfg.user_data = &buf;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.user_agent = "k85OS-device";

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGW(TAG, "uefi fetch: err=%d status=%d buf_len=%u", err, status, (unsigned)buf.len);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "uefi release fetch failed: err=%d status=%d", err, status);
        return false;
    }

    cJSON *root = cJSON_Parse(json_buf);
    if (!root) {
        ESP_LOGW(TAG, "uefi: cJSON_Parse failed, first 100 chars: %.100s", json_buf);
        return false;
    }

    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (!cJSON_IsArray(assets)) {
        ESP_LOGW(TAG, "uefi: no 'assets' array in response");
        cJSON_Delete(root);
        return false;
    }
    ESP_LOGW(TAG, "uefi: assets array size=%d", cJSON_GetArraySize(assets));

    int n = cJSON_GetArraySize(assets);
    int found = 0;
    for (int i = 0; i < n && found < max; i++) {
        cJSON *asset = cJSON_GetArrayItem(assets, i);
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        cJSON *dl_url = cJSON_GetObjectItem(asset, "browser_download_url");
        if (!cJSON_IsString(name) || !cJSON_IsString(dl_url)) continue;

        size_t nlen = strlen(name->valuestring);
        if (nlen <= 4 || strcasecmp(name->valuestring + nlen - 4, ".thm") != 0) continue;

        snprintf(out_names[found], 64, "%.63s", name->valuestring);
        snprintf(out_urls[found], 256, "%.255s", dl_url->valuestring);
        found++;
    }

    cJSON_Delete(root);
    *out_count = found;
    return found > 0;
}

bool k85_apprepo_download_file(const char *url, const char *dest_path) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.user_agent = "k85OS-device";

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_fetch_headers(client);

    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char buf[512];
    int r;
    bool ok = true;
    while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, r, f);
    }
    if (r < 0) ok = false;

    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}
