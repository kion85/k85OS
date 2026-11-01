#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "firmware_flash.h"
#include "esp_attr.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "core/heavy_lock.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "cJSON.h"
#include "ota_verify.h"
#include "mbedtls/sha256.h"

#include <cstdio>
#include <cstring>

static const char *TAG = "k85_fwflash";

#define K85_FW_OWNER "kion85"
#define K85_FW_REPO  "k85OS"

static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t *s_ota_partition = nullptr;
static bool s_stream_active = false;
static mbedtls_sha256_context s_hash_ctx;

const char *k85_fwflash_free_slot_label(void) {
    const esp_partition_t *p = esp_ota_get_next_update_partition(nullptr);
    return p ? p->label : "?";
}

// K85SEC4: after a failed or aborted write the slot holds unverified bytes: destroy the image header
// so that nobody can activate it later through "Alt Firmware" / the BIOS boot-slot menu.
static void invalidate_slot(void) {
    if (s_ota_partition) esp_partition_erase_range(s_ota_partition, 0, 4096);
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

    mbedtls_sha256_init(&s_hash_ctx);
    mbedtls_sha256_starts(&s_hash_ctx, 0);

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
    mbedtls_sha256_update(&s_hash_ctx, data, len);
    return true;
}

bool k85_fwflash_stream_end_verified(const char *signature_hex) {
    if (!s_stream_active) return false;

    uint8_t hash[32];
    mbedtls_sha256_finish(&s_hash_ctx, hash);
    mbedtls_sha256_free(&s_hash_ctx);

    if (!signature_hex || !k85_ota_verify_signature_hex(hash, sizeof(hash), signature_hex)) {
        ESP_LOGE(TAG, "firmware signature verification FAILED - refusing to keep this image");
        s_stream_active = false;
        esp_ota_abort(s_ota_handle); invalidate_slot();
        return false;
    }
    ESP_LOGI(TAG, "firmware signature verified OK");

    s_stream_active = false;
    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %d", (int)err);
        return false;
    }
    ESP_LOGI(TAG, "Firmware written to free slot: %s", s_ota_partition ? s_ota_partition->label : "?");
    return true;
}

bool k85_fwflash_stream_end(void) {
    ESP_LOGW(TAG, "k85_fwflash_stream_end() called without signature - rejecting by policy");
    if (s_stream_active) {
        s_stream_active = false;
        mbedtls_sha256_free(&s_hash_ctx);
        esp_ota_abort(s_ota_handle); invalidate_slot();
    }
    return false;
}

void k85_fwflash_stream_abort(void) {
    if (!s_stream_active) return;
    s_stream_active = false;
    mbedtls_sha256_free(&s_hash_ctx);
    esp_ota_abort(s_ota_handle); invalidate_slot();
}

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
        } else { buf->len = buf->cap; }  // K85SEC4: overflow, stop appending
    }
    return ESP_OK;
}

bool k85_fwflash_list_available(char out_names[][64], char out_urls[][256], char out_sig_urls[][256], int max, int *out_count) {
    *out_count = 0;

    char url[160];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases/latest", K85_FW_OWNER, K85_FW_REPO);

    static EXT_RAM_BSS_ATTR char json_buf[32768];
    HttpBuf buf = { json_buf, 0, sizeof(json_buf) };
    json_buf[0] = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_event_handler;
    cfg.user_data = &buf;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size_tx = 2048;  // K85NET1: long GitHub redirect URLs
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
        out_sig_urls[found][0] = 0;

        char sig_name[70];
        snprintf(sig_name, sizeof(sig_name), "%s.sig", name->valuestring);
        for (int j = 0; j < n; j++) {
            cJSON *sig_asset = cJSON_GetArrayItem(assets, j);
            cJSON *sig_name_j = cJSON_GetObjectItem(sig_asset, "name");
            cJSON *sig_url_j = cJSON_GetObjectItem(sig_asset, "browser_download_url");
            if (cJSON_IsString(sig_name_j) && cJSON_IsString(sig_url_j) &&
                strcmp(sig_name_j->valuestring, sig_name) == 0) {
                snprintf(out_sig_urls[found], 256, "%.255s", sig_url_j->valuestring);
                break;
            }
        }
        found++;
    }
    cJSON_Delete(root);
    *out_count = found;
    return found > 0;
}

static bool download_small_text(const char *url, char *out, size_t out_size) {
    HttpBuf buf = { out, 0, out_size - 1 };
    out[0] = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_event_handler;
    cfg.user_data = &buf;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size_tx = 2048;  // K85NET1: long GitHub redirect URLs
    cfg.timeout_ms = 8000;
    cfg.user_agent = "k85OS-device";

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) {
        out[--len] = 0;
    }
    return err == ESP_OK && status == 200 && len > 0;
}

bool k85_fwflash_from_url(const char *bin_url, const char *sig_url, k85_fwflash_progress_cb cb) {
    K85HeavyLockGuard heavy_lock(20000);
    if (!heavy_lock.held) return false;

    if (!sig_url || !sig_url[0]) {
        ESP_LOGE(TAG, "no signature URL provided - refusing unsigned firmware");
        return false;
    }

    char signature_hex[136];
    if (!download_small_text(sig_url, signature_hex, sizeof(signature_hex))) {
        ESP_LOGE(TAG, "failed to download signature file");
        return false;
    }

    if (!k85_fwflash_stream_begin()) return false;

    esp_http_client_config_t cfg = {};
    cfg.url = bin_url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size_tx = 2048;  // K85NET1: long GitHub redirect URLs
    cfg.timeout_ms = 30000;

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
    return k85_fwflash_stream_end_verified(signature_hex);
}

// ===================== K85TP1: third-party firmware install =====================
// Writes an UNSIGNED app image into the free OTA slot (never activates it). Local menu only, never
// reachable from the web upload. Accepts a plain app .bin or a full flash image (bootloader + partition
// table + app): in that case only the app part is written.
#include <cstdio>
#include <cstring>
#include "esp_heap_caps.h"
#include "list_menu.h"
#include "status_bar.h"
#include "text_input.h"
#include "M5Unified.h"
#include "theme.h"
#include "common.h"
#include "input.h"

#define TP_HEAD 0x8C00u

struct TpCtx {
    uint8_t *head;
    uint32_t head_len;
    bool decided, began, failed;
    const char *err;
    uint32_t start, limit, pos, written, seen, total, slot_size;
    mbedtls_sha256_context sha_all;
    k85_fwflash_progress_cb cb;
};

static void tp_msg(char *info, size_t cap, const char *m) {
    if (info && cap) snprintf(info, cap, "%s", m);
}

static void tp_hex(const uint8_t *h, char *out) {
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = d[h[i] >> 4]; out[2 * i + 1] = d[h[i] & 15]; }
    out[64] = 0;
}

// d is the next chunk of the file; only bytes inside [start, limit) go to the slot
static bool tp_stream(TpCtx *c, const uint8_t *d, size_t n) {
    uint32_t p = c->pos, end = p + (uint32_t)n;
    c->pos = end;
    uint32_t from = p < c->start ? c->start : p;
    uint32_t to = end > c->limit ? c->limit : end;
    if (to > from) {
        if (c->written == 0 && d[from - p] != 0xE9) { c->err = "not an ESP app image"; c->failed = true; return false; }
        if (!k85_fwflash_stream_write(d + (from - p), to - from)) { c->err = "flash write failed"; c->failed = true; return false; }
        c->written += to - from;
    }
    return true;
}

static bool tp_decide(TpCtx *c) {
    c->decided = true;
    const uint8_t *h = c->head;
    uint32_t start = 0, limit = 0xFFFFFFFFu;
    if (c->head_len >= 0x8020 && h[0x8000] == 0xAA && h[0x8001] == 0x50) {
        bool found = false;
        for (uint32_t o = 0x8000; o + 32 <= c->head_len; o += 32) {
            if (h[o] != 0xAA || h[o + 1] != 0x50) break;
            if (h[o + 2] == 0x00) {
                uint32_t off, sz;
                memcpy(&off, h + o + 4, 4);
                memcpy(&sz, h + o + 8, 4);
                start = off;
                limit = (sz > 0xFFFFFFFFu - off) ? 0xFFFFFFFFu : off + sz;
                found = true;
                break;
            }
        }
        if (!found) { c->err = "no app in the image"; c->failed = true; return false; }
        ESP_LOGW(TAG, "third-party: full flash image, app at 0x%x", (unsigned)start);
    } else if (c->total && c->total > c->slot_size) {
        c->err = "bigger than the slot";
        c->failed = true;
        return false;
    }
    if (limit - start > c->slot_size) limit = start + c->slot_size;
    c->start = start;
    c->limit = limit;
    if (!k85_fwflash_stream_begin()) { c->err = "cannot open the free slot"; c->failed = true; return false; }
    c->began = true;
    c->pos = 0;
    uint8_t tmp[512];
    for (uint32_t o = 0; o < c->head_len; o += sizeof(tmp)) {
        uint32_t m = c->head_len - o;
        if (m > sizeof(tmp)) m = sizeof(tmp);
        memcpy(tmp, c->head + o, m);
        if (!tp_stream(c, tmp, m)) return false;
    }
    return true;
}

static void tp_feed(TpCtx *c, const uint8_t *d, size_t n) {
    mbedtls_sha256_update(&c->sha_all, d, n);
    c->seen += (uint32_t)n;
    if (!c->decided) {
        uint32_t take = TP_HEAD - c->head_len;
        if (take > n) take = (uint32_t)n;
        memcpy(c->head + c->head_len, d, take);
        c->head_len += take;
        d += take;
        n -= take;
        if (c->head_len < TP_HEAD) return;
        if (!tp_decide(c)) return;
    }
    if (n) tp_stream(c, d, n);
}

static esp_err_t tp_http_event(esp_http_client_event_t *evt) {
    TpCtx *c = (TpCtx *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (esp_http_client_get_status_code(evt->client) != 200) return ESP_OK;
        if (c->failed) return ESP_OK;
        if (!c->total) {
            int64_t cl = esp_http_client_get_content_length(evt->client);
            if (cl > 0) c->total = (uint32_t)cl;
        }
        tp_feed(c, (const uint8_t *)evt->data, (size_t)evt->data_len);
        if (c->cb && c->total) c->cb((int)(((uint64_t)c->seen * 100) / c->total));
    }
    return ESP_OK;
}

// info: SHA-256 hex of the downloaded file on success (needs 65+ bytes), error text on failure
bool k85_fwflash_install_thirdparty(const char *url, k85_fwflash_progress_cb cb, char *info, size_t info_cap) {
    if (info && info_cap) info[0] = 0;
    K85HeavyLockGuard heavy_lock(20000);
    if (!heavy_lock.held) { tp_msg(info, info_cap, "device is busy"); return false; }
    if (!url || strncmp(url, "https://", 8) != 0) { tp_msg(info, info_cap, "https links only"); return false; }
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    if (!slot) { tp_msg(info, info_cap, "no free slot"); return false; }

    TpCtx ctx = {};
    TpCtx *c = &ctx;
    c->head = (uint8_t *)heap_caps_malloc(TP_HEAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->head) { tp_msg(info, info_cap, "no PSRAM for the buffer"); return false; }
    c->slot_size = slot->size;
    c->cb = cb;
    mbedtls_sha256_init(&c->sha_all);
    mbedtls_sha256_starts(&c->sha_all, 0);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = tp_http_event;
    cfg.user_data = c;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size_tx = 2048;  // K85NET1: long GitHub redirect URLs
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 4096;
    cfg.user_agent = "k85OS-device";

    bool ok = false;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        tp_msg(info, info_cap, "http init failed");
    } else {
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "third-party download: err=%s status=%d seen=%u written=%u failed=%d reason=%s", esp_err_to_name(err), status, (unsigned)c->seen, (unsigned)c->written, (int)c->failed, c->err ? c->err : "-");  // K85NET2
        if (err == ESP_OK && status == 200 && !c->failed && !c->decided && c->head_len) tp_decide(c);
        ok = (err == ESP_OK && status == 200 && !c->failed && c->written > 0);
        if (!ok) {
            if (c->began) k85_fwflash_stream_abort();
            if (c->err) tp_msg(info, info_cap, c->err);
            else if (err != ESP_OK) tp_msg(info, info_cap, "download error");
            else if (status != 200) tp_msg(info, info_cap, "server said not 200");
            else tp_msg(info, info_cap, "empty file");
        } else {
            uint8_t hash[32];
            mbedtls_sha256_finish(&c->sha_all, hash);
            s_stream_active = false;
            mbedtls_sha256_free(&s_hash_ctx);
            esp_err_t e = esp_ota_end(s_ota_handle);
            if (e != ESP_OK) {
                invalidate_slot();
                ok = false;
                tp_msg(info, info_cap, "image check failed");
                ESP_LOGE(TAG, "third-party image rejected by esp_ota_end: %d", (int)e);
            } else if (info && info_cap >= 65) {
                tp_hex(hash, info);
            }
        }
    }
    mbedtls_sha256_free(&c->sha_all);
    heap_caps_free(c->head);
    return ok;
}

// ---------- K85TP2: GitHub release picker + menu ----------
#define TP_MAX_ASSETS 40

struct TpAsset { char name[56]; char url[256]; uint32_t size; };
struct TpJson { char *buf; size_t len, cap; };

static esp_err_t tp_json_event(esp_http_client_event_t *evt) {
    TpJson *j = (TpJson *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && esp_http_client_get_status_code(evt->client) == 200) {
        size_t room = j->cap - 1 - j->len;
        size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        memcpy(j->buf + j->len, evt->data, n);
        j->len += n;
        j->buf[j->len] = 0;
    }
    return ESP_OK;
}

// p points just after a JSON key: skip ": " and return the start of the value
static const char *tp_value(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return nullptr;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// light text scan instead of a full JSON parse (a big release would not fit into internal RAM as a cJSON tree)
static int tp_scan_assets(const char *js, TpAsset *out, int max, char *tag, size_t tag_cap) {
    const char *tg = strstr(js, "\"tag_name\"");
    if (tg && tag && tag_cap) {
        const char *v = tp_value(tg + 10);
        if (v && *v == '"') {
            v++;
            size_t i = 0;
            while (v[i] && v[i] != '"' && i + 1 < tag_cap) { tag[i] = v[i]; i++; }
            tag[i] = 0;
        }
    }
    const char *key = "\"browser_download_url\"";
    const size_t klen = strlen(key);
    const char *p = js, *prev_end = js;
    int count = 0;
    while (count < max && (p = strstr(p, key)) != nullptr) {
        const char *v = tp_value(p + klen);
        if (!v || *v != '"') { p += klen; continue; }
        const char *u = v + 1;
        const char *q = strchr(u, '"');
        if (!q) break;
        size_t ul = (size_t)(q - u);
        uint32_t size = 0;
        const char *s = nullptr, *scan = prev_end;
        while ((scan = strstr(scan, "\"size\"")) != nullptr && scan < p) { s = scan; scan += 6; }
        if (s) { const char *sv = tp_value(s + 6); if (sv) size = (uint32_t)strtoul(sv, nullptr, 10); }
        prev_end = q;
        p = q;
        if (ul < 5 || ul >= sizeof(out[0].url)) continue;
        if (!(u[ul - 4] == '.' && (u[ul - 3] | 32) == 'b' && (u[ul - 2] | 32) == 'i' && (u[ul - 1] | 32) == 'n')) continue;
        memcpy(out[count].url, u, ul);
        out[count].url[ul] = 0;
        const char *slash = strrchr(out[count].url, '/');
        snprintf(out[count].name, sizeof(out[count].name), "%.55s", slash ? slash + 1 : out[count].url);
        out[count].size = size;
        count++;
    }
    return count;
}

static bool tp_repo_ok(const char *r) {
    if (!r || !strchr(r, '/') || strlen(r) > 80 || strstr(r, "..")) return false;
    for (const char *p = r; *p; p++) {
        char ch = *p;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '/';
        if (!ok) return false;
    }
    return true;
}

static int tp_list_github(const char *repo, TpAsset *out, int max, char *tag, size_t tag_cap, char *err, size_t err_cap) {
    if (tag && tag_cap) tag[0] = 0;
    if (!tp_repo_ok(repo)) { tp_msg(err, err_cap, "use owner/repo"); return 0; }
    char url[160];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", repo);
    TpJson j = {};
    j.cap = 512 * 1024;
    j.buf = (char *)heap_caps_malloc(j.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j.buf) { tp_msg(err, err_cap, "no PSRAM"); return 0; }
    j.buf[0] = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = tp_json_event;
    cfg.user_data = &j;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size_tx = 2048;  // K85NET1: long GitHub redirect URLs
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 4096;
    cfg.user_agent = "k85OS-device";

    int count = 0;
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) {
        tp_msg(err, err_cap, "http init failed");
    } else {
        esp_http_client_set_header(cl, "Accept", "application/vnd.github+json");
        esp_err_t e = esp_http_client_perform(cl);
        int st = esp_http_client_get_status_code(cl);
        esp_http_client_cleanup(cl);
        if (e != ESP_OK) tp_msg(err, err_cap, "network error");
        else if (st == 404) tp_msg(err, err_cap, "repo or release not found");
        else if (st == 403 || st == 429) tp_msg(err, err_cap, "GitHub limit, try later");
        else if (st != 200) snprintf(err, err_cap, "server said %d", st);
        else {
            count = tp_scan_assets(j.buf, out, max, tag, tag_cap);
            if (count == 0) tp_msg(err, err_cap, "no .bin files in release");
        }
    }
    heap_caps_free(j.buf);
    return count;
}

// ---------- menu ----------
struct TpEntry { const char *name; const char *repo; const char *hint; };
static const TpEntry TP_CATALOG[] = {
    // { "Name", "owner/repo", "text the file name must contain" }   ("s3" = ESP32-S3 builds)
    { "Nemo", "n0xa/m5stick-nemo", "cardputer" },
    { "WLED", "wled/WLED", "s3" },
};
#define TP_CATALOG_COUNT (int)(sizeof(TP_CATALOG) / sizeof(TP_CATALOG[0]))

static void tp_wait_ab(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// K85TP6: compact screens, S3 filter, short labels
static void tp_screen(const char *text) {
    uint32_t bg = k85_get_bg();
    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextWrap(true);
    M5.Display.setTextColor(0xFFFFFF, bg);
    M5.Display.setCursor(3, 16);
    M5.Display.print(text);
    k85_status_bar_overlay();
}

static void tp_progress(int pc) {
    static int last = -10;
    if (pc < last || pc - last >= 5) {
        last = pc;
        char b[48];
        snprintf(b, sizeof(b), "Writing... %d pct\n\nDo not power off", pc);
        tp_screen(b);
    }
}

// B released alone = yes. Anything that involves both buttons is an exit gesture, never a confirmation.
static bool tp_ask(const char *text) {
    tp_screen(text);
    bool chord = false;
    while (true) {
        k85_input_update();
        bool a = M5.BtnA.isPressed(), b = M5.BtnB.isPressed();
        if (a && b) {
            chord = true;
            if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        }
        if (M5.BtnB.wasReleased() && !chord) return true;
        if (!a && !b) chord = false;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void tp_confirm_and_install(const char *url, const char *full_name, uint32_t size) {
    char sz[16];
    if (size) snprintf(sz, sizeof(sz), "%uK", (unsigned)(size / 1024)); else snprintf(sz, sizeof(sz), "?");
    char q[220];
    snprintf(q, sizeof(q), "%.60s\n\n%s -> slot %s\nUNSIGNED: install only\nwhat you trust.\n\nB = install\nA+B = cancel", full_name, sz, k85_fwflash_free_slot_label());
    if (!tp_ask(q)) return;
    tp_screen("Downloading...\n\nDo not power off");
    char info[96];
    bool ok = k85_fwflash_install_thirdparty(url, tp_progress, info, sizeof(info));
    char msg[200];
    if (ok) {
        ESP_LOGW(TAG, "third-party firmware installed, SHA-256 of the file: %s", info);
        snprintf(msg, sizeof(msg), "Installed to %s\nsha256 %.12s...\n\nBIOS -> Boot options\n-> Alt Firmware\n\nA+B = back", k85_fwflash_free_slot_label(), info);
    } else {
        snprintf(msg, sizeof(msg), "Failed:\n%.60s\n\nA+B = back", info);
    }
    tp_screen(msg);
    tp_wait_ab();
}

static bool tp_has_ci(const char *s, const char *needle) {
    if (!needle || !needle[0]) return true;
    size_t n = strlen(needle);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && ((s[i] | 32) == (needle[i] | 32))) i++;
        if (i == n) return true;
    }
    return false;
}

// short list labels: drop ".bin", cut the prefix shared by all files, max 15 characters, "!" = bigger than the slot
static void tp_make_labels(const TpAsset *as, const int *idx, int m, uint32_t cap, char lbl[][24]) {
    static char nm[TP_MAX_ASSETS][56];
    for (int i = 0; i < m; i++) {
        snprintf(nm[i], sizeof(nm[i]), "%.55s", as[idx[i]].name);
        size_t l = strlen(nm[i]);
        if (l > 4 && nm[i][l - 4] == '.') nm[i][l - 4] = 0;
    }
    size_t cut = 0;
    if (m > 1) {
        size_t L = strlen(nm[0]);
        for (int i = 1; i < m; i++) {
            size_t k = 0;
            while (k < L && nm[i][k] == nm[0][k]) k++;
            L = k;
        }
        while (L > 0 && nm[0][L - 1] != '_' && nm[0][L - 1] != '-' && nm[0][L - 1] != '.' && nm[0][L - 1] != ' ') L--;
        cut = L;
    }
    for (int i = 0; i < m; i++) {
        const char *s = nm[i];
        if (cut && strlen(s) > cut) s += cut;
        bool big = cap && as[idx[i]].size > cap + 0x10000;
        snprintf(lbl[i], 24, "%s%.15s", big ? "!" : "", s);
    }
}

static bool tp_pick_asset(const char *repo, const char *hint, char *url_out, size_t url_cap, char *name_out, size_t name_cap, uint32_t *size_out) {
    tp_screen("Loading release...");
    TpAsset *as = (TpAsset *)heap_caps_calloc(TP_MAX_ASSETS, sizeof(TpAsset), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!as) { tp_screen("Out of memory\n\nA+B = back"); tp_wait_ab(); return false; }
    char tag[32] = "", err[48] = "";
    int n = tp_list_github(repo, as, TP_MAX_ASSETS, tag, sizeof(tag), err, sizeof(err));
    if (n <= 0) {
        char m[96];
        snprintf(m, sizeof(m), "%.60s\n\nA+B = back", err[0] ? err : "nothing found");
        tp_screen(m);
        tp_wait_ab();
        heap_caps_free(as);
        return false;
    }
    const esp_partition_t *sl = esp_ota_get_next_update_partition(nullptr);
    uint32_t cap = sl ? sl->size : 0;
    bool show_all = false, nomatch = false, ok = false;
    static char lbl[TP_MAX_ASSETS][24];
    while (true) {
        int idx[TP_MAX_ASSETS];
        int m = 0;
        for (int i = 0; i < n; i++) if (show_all || tp_has_ci(as[i].name, hint)) idx[m++] = i;
        if (m == 0) { show_all = true; nomatch = true; continue; }
        tp_make_labels(as, idx, m, cap, lbl);
        const char *items[TP_MAX_ASSETS + 2];
        for (int i = 0; i < m; i++) items[i] = lbl[i];
        int extra = show_all ? 0 : 1;
        if (!show_all) items[m] = "[show all files]";
        items[m + extra] = "Back";
        char title[24];
        snprintf(title, sizeof(title), "%.20s", nomatch ? "NO MATCH - ALL" : (show_all ? "ALL FILES" : (tag[0] ? tag : "RELEASE")));
        int sel = k85_run_list_menu(title, items, m + extra + 1, nullptr);
        if (sel < 0 || sel == m + extra) break;
        if (!show_all && sel == m) { show_all = true; continue; }
        snprintf(url_out, url_cap, "%s", as[idx[sel]].url);
        snprintf(name_out, name_cap, "%s", as[idx[sel]].name);
        *size_out = as[idx[sel]].size;
        ok = true;
        break;
    }
    heap_caps_free(as);
    return ok;
}

void k85_run_thirdparty_flash(void) {
    const char *items[TP_CATALOG_COUNT + 3];
    for (int i = 0; i < TP_CATALOG_COUNT; i++) items[i] = TP_CATALOG[i].name;
    items[TP_CATALOG_COUNT] = "Other GitHub repo";
    items[TP_CATALOG_COUNT + 1] = "Direct link";
    items[TP_CATALOG_COUNT + 2] = "Back";
    while (true) {
        int idx = k85_run_list_menu("OTHER FIRMWARE", items, TP_CATALOG_COUNT + 3, nullptr);
        if (idx < 0 || idx == TP_CATALOG_COUNT + 2) return;
        char url[256] = "";
        char name[56] = "";
        uint32_t size = 0;
        if (idx == TP_CATALOG_COUNT + 1) {
            if (!k85_text_input("Link (https):", "https://", url, sizeof(url))) continue;
            const char *sl = strrchr(url, '/');
            snprintf(name, sizeof(name), "%.55s", sl ? sl + 1 : url);
        } else {
            char repo[64] = "";
            const char *hint = "s3";
            if (idx < TP_CATALOG_COUNT) {
                snprintf(repo, sizeof(repo), "%s", TP_CATALOG[idx].repo);
                hint = TP_CATALOG[idx].hint;
            } else if (!k85_text_input("owner/repo:", "", repo, sizeof(repo))) {
                continue;
            }
            if (!tp_pick_asset(repo, hint, url, sizeof(url), name, sizeof(name), &size)) continue;
        }
        tp_confirm_and_install(url, name, size);
    }
}