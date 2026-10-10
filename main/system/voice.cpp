// K85V:psram - voice pack lives in PSRAM only (downloaded to RAM, not to flash)
#include "voice.h"
#include "M5Unified.h"
#include "esp_heap_caps.h"
#include "esp_log.h" // K85V:log
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

#ifndef K85_VOICE_URL
#define K85_VOICE_URL "https://github.com/kion85/k85OS/releases/download/voice-1/voice.pak"
#endif
#define VOICE_VOLUME 140                       // ~55% of 255
#define VOICE_START_CAP (3u * 1024u * 1024u)
#define VOICE_MAX_BYTES (4u * 1024u * 1024u)

struct VEntry { uint32_t hash, off, len; };
struct VMsg { char text[96]; uint32_t gen; };

static uint8_t *s_pak = nullptr;
static size_t s_pak_size = 0;
static const VEntry *s_idx = nullptr;
static uint32_t s_count = 0, s_rate = 8000;
static volatile bool s_on = false, s_off_pending = false, s_pack_running = false;
static volatile uint32_t s_gen = 0;
static volatile bool s_notify = false; // K85V:notify
static QueueHandle_t s_q = nullptr;

static uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)tolower((uint8_t)s[i]); h *= 16777619u; }
    return h;
}

static bool validate(const uint8_t *b, size_t n, uint32_t *count, uint32_t *rate) {
    if (!b || n < 12 || memcmp(b, "K85V", 4)) return false;
    uint32_t c, r;
    memcpy(&c, b + 4, 4); memcpy(&r, b + 8, 4);
    if (c == 0 || c > 8192 || r < 4000 || r > 48000) return false;
    if (12 + (size_t)c * sizeof(VEntry) > n) return false;
    const VEntry *e = (const VEntry *)(b + 12);
    for (uint32_t i = 0; i < c; i++) if ((size_t)e[i].off + e[i].len > n) return false;
    *count = c; *rate = r;
    return true;
}

// takes ownership of buf (must be heap_caps allocated)
static bool install(uint8_t *buf, size_t n) {
    uint32_t c, r;
    if (!validate(buf, n, &c, &r)) { heap_caps_free(buf); return false; }
    uint8_t *old = s_pak;
    s_pak = buf; s_pak_size = n;
    s_idx = (const VEntry *)(buf + 12); s_count = c; s_rate = r;
    if (old) { vTaskDelay(pdMS_TO_TICKS(100)); heap_caps_free(old); }
    return true;
}

static const VEntry *find_entry(uint32_t h) {
    uint32_t lo = 0, hi = s_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (s_idx[mid].hash == h) return &s_idx[mid];
        if (s_idx[mid].hash < h) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

static bool play_clip(const VEntry &e, uint32_t gen) {
    if (M5.Mic.isRunning()) return false; // K85V:spk - speaker released by mic / IR / etc.
    M5.Speaker.playRaw(s_pak + e.off, e.len, s_rate, false, 1, 0, true);
    while (M5.Speaker.isPlaying(0)) {
        if (gen != s_gen) { M5.Speaker.stop(0); vTaskDelay(pdMS_TO_TICKS(30)); return false; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return gen == s_gen;
}

// whole word if it is in the pack, otherwise letter by letter
static bool say_token(const char *w, size_t n, uint32_t gen) {
    if (const VEntry *e = find_entry(fnv(w, n))) return play_clip(*e, gen);
    for (size_t i = 0; i < n; i++) {
        const VEntry *e = find_entry(fnv(&w[i], 1));
        if (e && !play_clip(*e, gen)) return false;
    }
    return true;
}

static void voice_task(void *) {
    VMsg m;
    while (true) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) continue;
        if (!s_on || !s_pak || m.gen != s_gen) continue;
        const char *p = m.text;
        while (*p) {
            if (isalpha((uint8_t)*p)) {
                char w[32]; size_t n = 0;
                while (isalpha((uint8_t)*p)) { if (n < 31) w[n++] = (char)tolower((uint8_t)*p); p++; }
                if (!say_token(w, n, m.gen)) break;
            } else if (isdigit((uint8_t)*p)) {
                if (!say_token(p, 1, m.gen)) break;
                p++;
            } else p++;
        }
        if (s_off_pending) { s_on = false; s_off_pending = false; }
    }
}

static void ensure_init(void) {
    if (s_q) return;
    s_q = xQueueCreate(1, sizeof(VMsg));
    xTaskCreate(voice_task, "voice", 6144, nullptr, 3, nullptr);
}

void k85_voice_init(void) { ensure_init(); }
bool k85_voice_enabled(void) { return s_on; }
bool k85_voice_pack_present(void) { return s_pak != nullptr; }

void k85_voice_say(const char *t) {
    if (!s_on || !t) return;
    ensure_init();
    VMsg m; strncpy(m.text, t, sizeof(m.text) - 1); m.text[sizeof(m.text) - 1] = 0;
    m.gen = ++s_gen;
    xQueueOverwrite(s_q, &m);
}

void k85_voice_set(bool on) {
    ensure_init();
    if (on) {
        ESP_LOGI("k85_voice", "set on: pack=%d internal free=%u largest=%u", (int)(s_pak != nullptr), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        M5.Speaker.setVolume(VOICE_VOLUME);
        if (!s_pak) {                          // pack not in PSRAM yet: low beep, keep trying
            M5.Speaker.tone(300, 500);
            k85_voice_ensure_pack();
            return;
        }
        s_off_pending = false; s_on = true;
        k85_voice_say("on");
    } else if (s_on) {
        k85_voice_say("off");
        s_off_pending = true;
    }
}

struct DlCtx { uint8_t *buf; size_t len, cap; bool fail; };

static esp_err_t dl_evt(esp_http_client_event_t *e) {
    DlCtx *c = (DlCtx *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && c && !c->fail &&
        esp_http_client_get_status_code(e->client) == 200) {
        if (c->len + e->data_len > c->cap) {
            size_t ncap = c->cap + 256 * 1024;
            if (ncap > VOICE_MAX_BYTES) { c->fail = true; return ESP_OK; }
            uint8_t *nb = (uint8_t *)heap_caps_realloc(c->buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) { c->fail = true; return ESP_OK; }
            c->buf = nb; c->cap = ncap;
        }
        memcpy(c->buf + c->len, e->data, e->data_len);
        c->len += e->data_len;
    }
    return ESP_OK;
}

bool k85_voice_download(const char *url) {
    DlCtx ctx = {};
    ctx.cap = VOICE_START_CAP;
    ctx.buf = (uint8_t *)heap_caps_malloc(ctx.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx.buf) return false;
    esp_http_client_config_t cfg = {};
    cfg.url = url; cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = dl_evt; cfg.user_data = &ctx;
    cfg.timeout_ms = 20000; cfg.buffer_size = 4096; cfg.buffer_size_tx = 4096;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (c) {
        ok = esp_http_client_perform(c) == ESP_OK && esp_http_client_get_status_code(c) == 200 && !ctx.fail;
        esp_http_client_cleanup(c);
    }
    if (!ok || ctx.len == 0) { ESP_LOGW("k85_voice", "download failed: len=%u fail=%d", (unsigned)ctx.len, (int)ctx.fail); heap_caps_free(ctx.buf); return false; }
    uint8_t *shrunk = (uint8_t *)heap_caps_realloc(ctx.buf, ctx.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (shrunk) ctx.buf = shrunk;
    return install(ctx.buf, ctx.len);
}

static void pack_task(void *) {
    remove("/littlefs/voice.tmp");             // leftover from the old flash-based version
    remove("/littlefs/voice.pak");
    for (int i = 0; i < 60 && !s_pak; i++) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI("k85_voice", "download start: internal free=%u largest=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            if (k85_voice_download(K85_VOICE_URL)) { s_notify = true; ESP_LOGI("k85_voice", "download OK, pack in PSRAM, internal free=%u largest=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)); break; }
            ESP_LOGW("k85_voice", "download failed: internal free=%u largest=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
    s_pack_running = false;
    vTaskDelete(nullptr);
}

void k85_voice_ensure_pack(void) {
    if (s_pack_running || s_pak) return;
    s_pack_running = true;
    if (xTaskCreate(pack_task, "vpack", 10240, nullptr, 2, nullptr) != pdPASS) s_pack_running = false;
}

bool k85_voice_take_notify(void) {
    if (!s_notify) return false;
    s_notify = false;
    return true;
}

// Green screen for one second: "voice pack ready". Call from the UI thread only.
void k85_voice_flash_ready(void) {
    const uint32_t green = 0x00B050;
    int w = M5.Display.width(), h = M5.Display.height();
    M5.Display.fillScreen(green);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(0xFFFFFF, green);
    const char *l1 = "Voice pack";
    const char *l2 = "ready";
    M5.Display.setCursor((w - (int)strlen(l1) * 12) / 2, h / 2 - 20);
    M5.Display.print(l1);
    M5.Display.setCursor((w - (int)strlen(l2) * 12) / 2, h / 2 + 4);
    M5.Display.print(l2);
    M5.Speaker.tone(1047, 90);
    vTaskDelay(pdMS_TO_TICKS(120));
    M5.Speaker.tone(1568, 160);
    vTaskDelay(pdMS_TO_TICKS(1000));
}
