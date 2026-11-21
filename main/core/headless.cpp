// K85HL1: no-display mode (see headless.h). Consolidated build: multi-connection web server with keep-alive.
#include "headless.h"
#include "config.h"
#include "svc_cfg.h"
#include "fs_guard.h"
#include "lang.h"
#include "input.h"
#include "wifi.h"
#include "voice.h"

#include "M5Unified.h"
#include "lgfx/v1/panel/Panel_FrameBufferBase.hpp"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "k85_hl";

// 0 = off, 1 = auto (blank display id = no display), 2 = always.
// Used when /littlefs/k85_svc.cfg has no "headless" key. Set to 1 after the screen is repaired.
#define K85_HEADLESS_DEFAULT 0

#define HL_GPIO_A 11     /* StickS3 buttons, active low (same pins as in the bootloader hook) */
#define HL_GPIO_B 12
#define HL_STACK  8192
#define HL_POOL   4

// ---------------- RAM panel ----------------
static volatile uint32_t g_fb_wr = 0;   // counts drawing calls: the server compares buffers only when something was drawn

struct K85FbPanel : public lgfx::Panel_FrameBufferBase {
    void setup(uint8_t **lines, int w, int h, int rot) {
        auto c = config();
        c.panel_width = (uint16_t)w;   c.panel_height = (uint16_t)h;
        c.memory_width = (uint16_t)w;  c.memory_height = (uint16_t)h;
        c.offset_x = 0; c.offset_y = 0; c.offset_rotation = 0;
        c.readable = true; c.invert = false;
        config(c);
        _lines_buffer = lines;
        setColorDepth(lgfx::color_depth_t::rgb565_2Byte);
        setRotation((uint_fast8_t)rot);
    }
    // the UI never waits for a real display now: give the CPU back now and then (IDLE0 watchdog) and count the drawing
    static void bump() {
        g_fb_wr = g_fb_wr + 1;
        static int64_t last = 0;
        int64_t n = esp_timer_get_time();
        if (n - last > 300000) { last = n; vTaskDelay(1); }
    }
    void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t c) override {
        lgfx::Panel_FrameBufferBase::writeFillRectPreclipped(x, y, w, h, c); bump();
    }
    void writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, lgfx::pixelcopy_t *p, bool d) override {
        lgfx::Panel_FrameBufferBase::writeImage(x, y, w, h, p, d); bump();
    }
    void writeImageARGB(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, lgfx::pixelcopy_t *p) override {
        lgfx::Panel_FrameBufferBase::writeImageARGB(x, y, w, h, p); bump();
    }
    void writePixels(lgfx::pixelcopy_t *p, uint32_t len, bool d) override {
        lgfx::Panel_FrameBufferBase::writePixels(p, len, d); bump();
    }
    void writeBlock(uint32_t c, uint32_t len) override {
        lgfx::Panel_FrameBufferBase::writeBlock(c, len); bump();
    }
    void drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t c) override {
        lgfx::Panel_FrameBufferBase::drawPixelPreclipped(x, y, c); bump();
    }
    void copyRect(uint_fast16_t dx, uint_fast16_t dy, uint_fast16_t w, uint_fast16_t h, uint_fast16_t sx, uint_fast16_t sy) override {
        lgfx::Panel_FrameBufferBase::copyRect(dx, dy, w, h, sx, sy); bump();
    }
};

static bool s_active = false;
static bool s_started = false;
static bool s_pw_override = false;   // K85WPA3: A+B at power-on opens the AP for this boot even when a password is set
static uint8_t *s_fb_mem = nullptr;     // what the UI draws into
static uint8_t *s_prev = nullptr;       // what the browser has (last frame sent)
static uint8_t *s_rle = nullptr;
static int s_nw = 0, s_nh = 0;
static EXT_RAM_BSS_ATTR uint8_t *s_lines[400];
static K85FbPanel s_fb;
static StackType_t *s_stack = nullptr;
static StaticTask_t s_tcb;
static esp_netif_t *s_ap_netif = nullptr;

static uint32_t s_prev_hash = 0;
static int s_prev_rot = -1;
static bool s_prev_valid = false;
static int64_t s_last_frame = 0;
static int64_t s_gap = 70000;
static uint32_t s_seen_wr = 0;
static volatile bool s_bios_req = false;
static bool s_keep = true;              // keep the connection open after this response

bool k85_headless_active(void) { return s_active; }
bool k85_headless_bios_requested(void) { bool r = s_bios_req; s_bios_req = false; return r; }

void k85_mem_report(char *out, size_t n) {
    static long l_i = 0, l_b = 0, l_p = 0;
    static bool seen = false;
    multi_heap_info_t hi, hp;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_get_info(&hp, MALLOC_CAP_SPIRAM);
    long fi = (long)hi.total_free_bytes, bi = (long)hi.largest_free_block, fp = (long)hp.total_free_bytes;
    if (!seen) { l_i = fi; l_b = bi; l_p = fp; seen = true; }
    snprintf(out, n, "int free %ldK min %uK big %ldK\r\nblocks %u used %uK\r\npsram free %ldK big %uK\r\ndelta int %+ld big %+ld ps %+ld B\r\ntasks %u\r\n",
             fi / 1024, (unsigned)(hi.minimum_free_bytes / 1024), bi / 1024, (unsigned)hi.allocated_blocks, (unsigned)(hi.total_allocated_bytes / 1024),
             fp / 1024, (unsigned)(hp.largest_free_block / 1024), fi - l_i, bi - l_b, fp - l_p, (unsigned)uxTaskGetNumberOfTasks());
    l_i = fi; l_b = bi; l_p = fp;
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);   // per-region details go to the monitor
}

void k85_headless_log_ram(const char *tag) {
    ESP_LOGW(TAG, "%s: internal free %u KB, largest %u KB", tag, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

bool k85_headless_can_keep_wifi(void) {
    if (!s_active || !s_started) return false;
    size_t fi_ = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), lb_ = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); bool ok = fi_ >= 100 * 1024 && lb_ >= 24 * 1024;   // tested: with the AP up the BLE host runs out of internal RAM (assert in gatt_init), so WiFi is always stopped for BLE tools
    ESP_LOGW(TAG, "BLE start: internal free %u KB, largest %u KB -> %s", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024), ok ? "keeping WiFi/AP" : "stopping WiFi (not enough RAM)");
    return ok;
}

static int hl_mode(void) {
    char v[8];
    if (k85_svc_get_str("headless", v, sizeof(v))) {
        if (!strcmp(v, "on")) return 2;
        if (!strcmp(v, "auto")) return 1;
        if (!strcmp(v, "off")) return 0;
    }
    return K85_HEADLESS_DEFAULT;
}

static bool auto_detect_dead(void) {
    uint32_t id = 0;
    auto *p = M5.Display.getPanel();
    if (p) {
        M5.Display.startWrite();
        id = (uint32_t)p->readCommand(0x04, 1, 3);
        M5.Display.endWrite();
    }
    bool blank = (id == 0 || id == 0xFFFFFF);
    ESP_LOGI(TAG, "display id probe: 0x%06x%s", (unsigned)id, blank ? " (blank)" : "");
    if (!blank) { k85_svc_set_bool("disp_ok", true); return false; }
    return true;   // a blank id = nothing answers = no display
}

bool k85_headless_pre(void) {
    int mode = hl_mode();
    if (gpio_get_level((gpio_num_t)HL_GPIO_A) == 0 && gpio_get_level((gpio_num_t)HL_GPIO_B) == 0) {
        mode = 2;
        k85_svc_set_str("headless", "on");
        ESP_LOGW(TAG, "A+B held at boot: headless forced ON");
        s_pw_override = true;   // K85WPA3
    }
    bool go = (mode == 2);
    if (mode == 1) go = auto_detect_dead();
    ESP_LOGI(TAG, "headless mode=%d -> %s", mode, go ? "ACTIVE" : "off");
    if (!go) return false;

    int rot = (int)M5.Display.getRotation();
    int w = (int)M5.Display.width(), h = (int)M5.Display.height();
    if (rot & 1) std::swap(w, h);                 // native (rotation 0) size
    if (h > 400 || w * h <= 0) return false;
    size_t fbn = (size_t)w * h * 2;
    s_fb_mem = (uint8_t *)heap_caps_calloc(1, fbn, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_prev = (uint8_t *)heap_caps_calloc(1, fbn, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rle = (uint8_t *)heap_caps_calloc(1, fbn, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb_mem || !s_prev || !s_rle) { ESP_LOGE(TAG, "no memory for the frame buffers"); heap_caps_free(s_fb_mem); heap_caps_free(s_prev); heap_caps_free(s_rle); s_fb_mem = nullptr; s_prev = nullptr; s_rle = nullptr; return false; }  // K85FIX1
    for (int y = 0; y < h; y++) s_lines[y] = s_fb_mem + (size_t)y * w * 2;
    s_nw = w; s_nh = h;

    s_stack = (StackType_t *)heap_caps_malloc(HL_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);   // before WiFi eats the internal RAM
    s_fb.setup(s_lines, w, h, rot);
    M5.Display.setPanel(&s_fb);
    M5.Display.setColorDepth(lgfx::color_depth_t::rgb565_2Byte);
    M5.Display.setRotation(rot);
    M5.Display.fillScreen(0);

    // internal RAM is tiny: no background downloads while the web UI runs (in memory only, not saved)
    g_config.voice_dl_block = true;
    g_config.voice_auto_dl = false;
    g_config.ota_bg_check_enabled = false;

    k85_wifi_set_low_mem(true);                   // the next k85_wifi_init() uses the small-buffer profile
    s_active = true;
    return true;
}

static void beep(int hz, int ms) {
    if (M5.Speaker.isEnabled() && !g_config.sound_muted && g_config.sound_volume > 0) M5.Speaker.tone((float)hz, (uint32_t)ms);
}

// one A+B press made by the stick itself (pulls both button pins low like a finger): the "back to the system" button
static void pin_pulse(int ms) {
    for (int i = 0; i < 2; i++) {
        int g = i ? HL_GPIO_B : HL_GPIO_A;
        gpio_set_direction((gpio_num_t)g, GPIO_MODE_INPUT_OUTPUT_OD);
        gpio_set_level((gpio_num_t)g, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    for (int i = 0; i < 2; i++) {
        int g = i ? HL_GPIO_B : HL_GPIO_A;
        gpio_set_level((gpio_num_t)g, 1);
        gpio_set_direction((gpio_num_t)g, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)g, GPIO_PULLUP_ONLY);
    }
}

// ---------------- session ----------------
static bool s_sess = false, s_pend = false;
static char s_tok[17], s_sess_nonce[17], s_pend_nonce[17];
static int64_t s_sess_last = 0, s_pend_since = 0, s_both_since = 0;

static void rand_hex(char *out, int n) {
    uint8_t r[8];
    esp_fill_random(r, sizeof(r));
    for (int i = 0; i < n / 2 && i < 8; i++) snprintf(out + i * 2, 3, "%02x", r[i]);
    out[n] = 0;
}

// K85PT3: a gesture that no screen took by itself becomes a real button press:
// tap = B, swipe/wheel down = A (one press per step), swipe/wheel up = A held (previous item where the screen supports it)
static int64_t s_btn_end[2] = {0, 0};
static int s_a_queue = 0;
static int64_t s_next_a = 0;
static void pin_set(int idx, bool on) {
    int g = idx ? HL_GPIO_B : HL_GPIO_A;
    if (on) {
        gpio_set_direction((gpio_num_t)g, GPIO_MODE_INPUT_OUTPUT_OD);
        gpio_set_level((gpio_num_t)g, 0);
    } else {
        gpio_set_level((gpio_num_t)g, 1);
        gpio_set_direction((gpio_num_t)g, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)g, GPIO_PULLUP_ONLY);
    }
}
static void btn_pulse(int idx, int ms, int64_t now) { pin_set(idx, true); s_btn_end[idx] = now + (int64_t)ms * 1000; }
static void gesture_tick(int64_t now) {
    for (int i = 0; i < 2; i++) {
        if (s_btn_end[i] && now >= s_btn_end[i]) { pin_set(i, false); s_btn_end[i] = 0; }
    }
    int64_t age = 0;
    if (k85_pointer_age(&age) && age > 250000 && !s_btn_end[1]) {
        int x, y;
        if (k85_pointer_take(&x, &y)) btn_pulse(1, 130, now);
    }
    if (k85_scroll_age(&age) && age > 250000) {
        int n = k85_scroll_take();
        if (n > 0) s_a_queue += (n > 4 ? 4 : n);
        else if (n < 0 && !s_btn_end[0]) btn_pulse(0, 750, now);
    }
    if (s_a_queue > 0 && !s_btn_end[0] && now >= s_next_a) {
        btn_pulse(0, 90, now);
        s_a_queue--;
        s_next_a = now + 200000;
    }
}
static void tick(int64_t now) {
    gesture_tick(now);
    if (s_sess && now - s_sess_last > 300000000) {
        s_sess = false;
        ESP_LOGI(TAG, "session expired");
    }
    if (s_pend) {
        if (now - s_pend_since > 60000000) {
            s_pend = false;
        } else {
            bool both = gpio_get_level((gpio_num_t)HL_GPIO_A) == 0 && gpio_get_level((gpio_num_t)HL_GPIO_B) == 0;
            if (!both) s_both_since = 0;
            else if (!s_both_since) s_both_since = now;
            else if (now - s_both_since >= 300000) {
                rand_hex(s_tok, 16);
                snprintf(s_sess_nonce, sizeof(s_sess_nonce), "%s", s_pend_nonce);
                s_sess = true; s_pend = false; s_sess_last = now;
                s_prev_valid = false;             // the new browser gets a full frame
                beep(2200, 70);
                ESP_LOGI(TAG, "session granted");
            }
        }
    }
}

// ---------------- HTTP helpers ----------------
static bool send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len) {
        int n = send(fd, p, len > 1436 ? 1436 : len, 0);
        if (n <= 0) return false;
        p += n; len -= (size_t)n;
    }
    return true;
}
static void send_resp(int fd, const char *status, const char *ctype, const void *body, size_t len, const char *extra) {
    char h[400];
    bool no_body = (status[0] == '2' && status[1] == '0' && status[2] == '4');
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s\r\n", status, ctype);
    if (!no_body) n += snprintf(h + n, sizeof(h) - n, "Content-Length: %u\r\n", (unsigned)len);
    n += snprintf(h + n, sizeof(h) - n, "Cache-Control: no-store\r\nConnection: %s\r\nX-J: %d\r\n%s\r\n", s_keep ? "keep-alive" : "close", k85_joy_wanted() ? 1 : 0, extra ? extra : "");
    if (n > (int)sizeof(h) - 1) n = (int)sizeof(h) - 1;  // K85FIX4: a long "extra" header was truncated, n then pointed past the buffer
    send_all(fd, h, (size_t)n);
    if (len && !no_body) send_all(fd, body, len);
}
static void send_json(int fd, const char *j) { send_resp(fd, "200 OK", "application/json", j, strlen(j), nullptr); }
static void send_err(int fd, const char *status, const char *msg) { send_resp(fd, status, "text/plain", msg, strlen(msg), nullptr); }

static bool qparam(const char *q, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    const char *p = q;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < n) out[i++] = *p++;
            out[i] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 32) >= 'a' && (c | 32) <= 'f') return (c | 32) - 'a' + 10;
    return -1;
}
static void url_decode(const char *s, char *o, size_t n) {
    size_t i = 0;
    while (*s && i + 1 < n) {
        if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { o[i++] = (char)(hexv(s[1]) * 16 + hexv(s[2])); s += 3; }
        else if (*s == '+') { o[i++] = ' '; s++; }
        else o[i++] = *s++;
    }
    o[i] = 0;
}

// K85SEC8: constant-time string compare (session token / login nonce)
static bool hl_ct_eq(const char *a, const char *b) {
    const size_t la = strlen(a), lb = strlen(b);
    unsigned char d = (unsigned char)(la != lb);
    for (size_t i = 0; i < lb; i++) d |= (unsigned char)((unsigned char)a[i < la ? i : 0] ^ (unsigned char)b[i]);
    return lb > 0 && d == 0;
}

static bool authed(const char *q, int64_t now) {
    char t[24];
    if (!s_sess || !qparam(q, "t", t, sizeof(t)) || !hl_ct_eq(t, s_tok)) return false;  // K85SEC8
    s_sess_last = now;
    return true;
}

static uint32_t fnv(const uint8_t *p, size_t n) {
    const uint32_t *w = (const uint32_t *)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (n >> 2); i++) { h ^= w[i]; h *= 16777619u; h ^= h >> 15; }
    return h ? h : 1;
}

// RLE of RGB565 pixels: [count 1..255][2 bytes]; returns 0 when it does not fit into cap
static size_t rle_encode(const uint8_t *src, size_t npx, uint8_t *dst, size_t cap) {
    size_t o = 0, i = 0;
    while (i < npx) {
        uint8_t a = src[i * 2], b = src[i * 2 + 1];
        size_t j = i + 1;
        while (j < npx && j - i < 255 && src[j * 2] == a && src[j * 2 + 1] == b) j++;
        if (o + 3 > cap) return 0;
        dst[o++] = (uint8_t)(j - i); dst[o++] = a; dst[o++] = b;
        i = j;
    }
    return o;
}

static const char *PAGE = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>k85OS</title>
<style>
html,body{margin:0;height:100%;background:#000;overflow:hidden;-webkit-user-select:none;user-select:none}
canvas{position:fixed;left:0;top:0;width:100vw;height:100vh;object-fit:contain;image-rendering:pixelated;display:none;touch-action:none}
#m{position:fixed;left:0;right:0;top:40%;text-align:center;color:#9cf;font:16px monospace;padding:0 16px}
#t{position:fixed;left:0;right:0;bottom:8px;text-align:center;color:#8899aa;font:12px monospace;pointer-events:none}
#g{position:fixed;right:6px;top:6px;width:30px;height:30px;line-height:30px;text-align:center;border-radius:15px;background:#223;color:#9cf;opacity:.35;font-size:18px;display:none}
#p{display:none;position:fixed;right:6px;top:42px;width:176px;box-sizing:border-box;max-height:calc(100vh - 52px);max-height:calc(100dvh - 52px);overflow-y:auto;overscroll-behavior:contain;-webkit-overflow-scrolling:touch;touch-action:pan-y;z-index:30;background:rgba(16,16,24,.92);border:1px solid #446;border-radius:8px;padding:6px}/*K85PNL1*/
@media (max-height:520px){#p button{margin:3px 0;padding:5px;font-size:13px}#p .r button{padding:5px 0}#rm{font-size:10px}}
#p button{display:block;width:100%;margin:4px 0;padding:8px;background:#1c2038;color:#fff;border:1px solid #66a;border-radius:6px;font:14px monospace}
#p .r{display:flex;gap:4px;font:12px monospace;color:#889;align-items:center}
#p .r button{margin:0;padding:6px 0}
#rm{font:11px monospace;color:#889;white-space:pre-line;margin-top:4px}
#mu{display:none;z-index:30;position:fixed;left:0;right:0;bottom:0;max-height:60vh;overflow:auto;background:rgba(16,16,24,.96);border-top:1px solid #446;color:#ccd;font:14px monospace}
#mh{display:flex;justify-content:space-between;padding:8px 12px;color:#9cf;border-bottom:1px solid #334}
#mx{padding:0 8px}
#ml div{padding:11px 12px;border-bottom:1px solid #223}
#au{width:100%;display:block}
#jy{display:none;position:fixed;left:0;right:0;bottom:0;height:200px;pointer-events:none;z-index:5}
#jy>div{pointer-events:auto;position:absolute;-webkit-user-select:none;user-select:none;touch-action:none}
#js{left:3vw;bottom:14px;width:150px;height:150px;border-radius:75px;background:rgba(80,120,255,.18);border:2px solid rgba(150,190,255,.5)}
#jk{position:absolute;left:45px;top:45px;width:60px;height:60px;border-radius:30px;background:rgba(150,190,255,.6);pointer-events:none}
#ja{left:3vw;bottom:14px;width:150px;height:150px;display:none}
#ja b{position:absolute;width:48px;height:48px;line-height:48px;text-align:center;border-radius:10px;background:rgba(80,120,255,.3);border:1px solid #8af;color:#cde;font-size:22px;touch-action:none}
#jb{right:4vw;bottom:34px;width:96px;height:96px;line-height:96px;text-align:center;border-radius:48px;background:rgba(255,160,40,.35);border:2px solid #fa4;color:#fd8;font:bold 16px monospace}
#jm{left:3vw;bottom:172px;width:36px;height:28px;line-height:28px;text-align:center;border-radius:6px;background:rgba(80,120,255,.3);color:#cde;font-size:16px}
</style></head><body>
<div id="m">подключение...</div>
<canvas id="c" width="240" height="135"></canvas>
<div id="t"></div>
<div id="g">&#9881;</div>
<div id="jy"><div id="js"><div id="jk"></div></div><div id="ja"><b data-d="u" style="left:51px;top:0">&#9650;</b><b data-d="d" style="left:51px;top:102px">&#9660;</b><b data-d="l" style="left:0;top:51px">&#9664;</b><b data-d="r" style="left:102px;top:51px">&#9654;</b></div><div id="jb">BOOST</div><div id="jm">&#8646;</div></div>
<div id="mu"><div id="mh"><span id="md"></span><b id="mx">&#10005;</b></div><div id="ml"></div><audio id="au" controls preload="none"></audio></div>
<div id="p">
<button id="bu">Загрузить файл</button><input type="file" id="fi" style="display:none">
<button id="bm">Музыка</button>
<button id="bk">В систему (A+B)</button>
<button id="bb">BIOS</button>
<button id="br">Reboot</button>
<button id="bf">Полный экран</button>
<button id="bw">Пароль Wi-Fi</button>
<button id="bc">Цвета</button>
<div id="rm"></div>
<div class="r">режим<button data-m="on">on</button><button data-m="off">off</button></div>
<div class="r">язык<button data-l="ru">RU</button><button data-l="en">EN</button></div>
</div>
<script>
var N=localStorage.k85n;
if(!N){N=(Math.random().toString(36)+'00000000').slice(2,10)+Date.now().toString(36).slice(-4);localStorage.k85n=N}
var T=null,V=0,LR=0,swap=localStorage.k85s==='1';
var cv=document.getElementById('c'),cx=cv.getContext('2d'),mm=document.getElementById('m'),tt=document.getElementById('t'),gg=document.getElementById('g'),pp=document.getElementById('p');
var px=null,PW=0,PH=0,tto=null;
var lut=new Uint32Array(65536);
for(var q=0;q<65536;q++){var r5=((q>>11)&31)*255/31|0,g6=((q>>5)&63)*255/63|0,b5=(q&31)*255/31|0;lut[q]=(255<<24)|(b5<<16)|(g6<<8)|r5}
function msg(s){mm.style.display=s?'block':'none';mm.textContent=s||''}
function tip(s,ms){tt.textContent=s;clearTimeout(tto);tto=setTimeout(function(){tt.textContent=''},ms||4000)}
async function auth(){
 try{
  var j=await (await fetch('/api/auth?n='+N)).json();
  if(j.s==='ok'){T=j.t;V=0;px=null;msg('');gg.style.display='block';loop();return}
  if(j.s==='busy')msg('Занято: стик уже подключён к другому браузеру');
  else msg('Нажмите A и B вместе на самом стике ('+j.left+' с)');
 }catch(e){msg('нет связи со стиком')}
 setTimeout(auth,1000);
}
function render(nw,nh,r){
 var sw=(r&1)?nh:nw,sh=(r&1)?nw:nh;
 if(cv.width!==sw||cv.height!==sh){cv.width=sw;cv.height=sh}
 cv.style.display='block';
 var im=cx.createImageData(sw,sh),d=new Uint32Array(im.data.buffer),f=((1<<r)&0x96)!==0;
 for(var y=0;y<sh;y++)for(var x=0;x<sw;x++){
  var bx=x,by=y;
  if(f)by=sh-1-by;
  if(r&2)bx=sw-1-bx;
  if(r&1){var t=bx;bx=by;by=t}
  var p=px[by*nw+bx];
  if(swap)p=((p&255)<<8)|(p>>8);
  d[y*sw+x]=lut[p];
 }
 cx.putImageData(im,0,0);
}
function apply(a,nw,nh,r,enc,y0,y1){
 if(!px||PW!==nw||PH!==nh){px=new Uint16Array(nw*nh);PW=nw;PH=nh}
 var base=y0*nw,i=base,end=(y1+1)*nw;
 if(enc){var o=0;while(o+2<a.length&&i<end){var c=a[o],v=(a[o+1]<<8)|a[o+2];o+=3;while(c--&&i<end)px[i++]=v}}
 else{for(;i<end;i++){var k=(i-base)*2;px[i]=(a[k]<<8)|a[k+1]}}
 LR=r;render(nw,nh,r);
}
async function loop(){
 while(T){
  var dly=60,ac=new AbortController(),tm=setTimeout(function(){ac.abort()},5000);
  try{
   var r=await fetch('/api/f?t='+T+'&v='+V,{signal:ac.signal});
   var buf=null;
   if(r.status===200)buf=await r.arrayBuffer();
   clearTimeout(tm);
   if(r.headers.get('X-J')!==null)setJoy(r.headers.get('X-J')==='1');
    if(r.status===401){setJoy(false);T=null;cv.style.display='none';gg.style.display='none';pp.style.display='none';auth();return}
   if(buf){
    var h=function(n){return +r.headers.get(n)};
    V=h('X-V');
    apply(new Uint8Array(buf),h('X-W'),h('X-H'),h('X-R'),r.headers.get('X-E')==='1',h('X-Y0'),h('X-Y1'));
    dly=10;
   }
  }catch(e){clearTimeout(tm);msg('связь потеряна');T=null;auth();return}
  await new Promise(function(s){setTimeout(s,dly)});
 }
}
function api(path,cb){fetch(path+(path.indexOf('?')<0?'?':'&')+'t='+T).then(cb||function(){}).catch(function(){})}
function fs(){var d=document,e=d.documentElement;if(d.fullscreenElement||d.webkitFullscreenElement){(d.exitFullscreen||d.webkitExitFullscreen).call(d)}else{var f=e.requestFullscreen||e.webkitRequestFullscreen;if(f)f.call(e)}pp.style.display='none'}
gg.onclick=function(){pp.style.display=pp.style.display==='block'?'none':'block'};
document.getElementById('bf').onclick=fs;
document.getElementById('bc').onclick=function(){swap=!swap;localStorage.k85s=swap?'1':'0';if(px)render(PW,PH,LR);tip('цвета: '+(swap?'swap':'обычные'))};
document.getElementById('bb').onclick=function(){if(T&&confirm('Открыть BIOS на стике?'))api('/api/bios',function(){pp.style.display='none';tip('BIOS откроется, когда стик вернётся в главное меню',6000)})};
document.getElementById('bk').onclick=function(){if(T)api('/api/back',function(){pp.style.display='none';tip('отправлено A+B (назад)',3000)})};
document.getElementById('br').onclick=function(){if(T&&confirm('Перезагрузить стик?'))api('/api/reboot')};
[].forEach.call(document.querySelectorAll('[data-m]'),function(b){b.onclick=function(){var m=b.getAttribute('data-m');api('/api/mode?m='+m,function(){tip('режим без экрана после перезагрузки: '+m,5000)})}});
var fi=document.getElementById('fi');
document.getElementById('bu').onclick=function(){fi.value='';fi.click()};
fi.onchange=function(){
 var f=fi.files[0];if(!f||!T)return;
 var n=prompt('Имя на стике (можно с папкой, латиницей, например scripts/a.lua):',f.name);if(!n)return;
 var x=new XMLHttpRequest();x.open('POST','/api/up?t='+T+'&name='+encodeURIComponent(n));
 x.upload.onprogress=function(e){if(e.lengthComputable)tip('загрузка '+Math.round(e.loaded*100/e.total)+'%',60000)};
 x.onload=function(){tip(x.status===200?'файл загружен: '+n:'ошибка '+x.status+' '+x.responseText.slice(0,40),6000)};
 x.onerror=function(){tip('ошибка связи при загрузке',6000)};
 x.send(f);pp.style.display='none';
};
var MD='',mu=document.getElementById('mu');
function lsdir(d){
 MD=d;
 fetch('/api/ls?t='+T+'&dir='+encodeURIComponent(d)).then(function(r){return r.json()}).then(function(j){
  var l=document.getElementById('ml');l.textContent='';
  document.getElementById('md').textContent='/'+d;
  var add=function(txt,fn){var e=document.createElement('div');e.textContent=txt;e.onclick=fn;l.appendChild(e)};
  if(d)add('.. вверх',function(){lsdir(d.lastIndexOf('/')<0?'':d.slice(0,d.lastIndexOf('/')))});
  j.e.forEach(function(x){
   var p=d?d+'/'+x.n:x.n;
   if(x.k)add('['+x.n+']',function(){lsdir(p)});
   else add(x.n,function(){var a=document.getElementById('au');a.src='/api/audio?t='+T+'&p='+encodeURIComponent(p);var pr=a.play();if(pr&&pr.catch)pr.catch(function(){})});
  });
  if(!j.e.length)add('(нет mp3/wav)',function(){});
 }).catch(function(){tip('не удалось получить список',4000)});
}
document.getElementById('bm').onclick=function(){if(!T)return;pp.style.display='none';mu.style.display='block';lsdir(MD)};
document.getElementById('mx').onclick=function(){mu.style.display='none'};
document.getElementById('bw').onclick=function(){if(!T)return;var p=prompt('Пароль точки Wi-Fi (8-63 символа, WPA2/WPA3). Пусто = убрать пароль. Применится после перезагрузки стика. Если сеть сейчас открыта, пароль один раз уйдёт по ней открытым текстом.','');if(p===null)return;if(p&&(p.length<8||p.length>63)){tip('нужно 8-63 символа',4000);return}api('/api/ap?p='+encodeURIComponent(p),function(r){r.json().then(function(j){pp.style.display='none';tip(j.s==='ok'?(p?'пароль задан, действует после перезагрузки':'пароль убран, сеть будет открытой после перезагрузки'):'ошибка: '+(j.s||''),6000)})})};
var rm=document.getElementById('rm');
function memq(){if(T&&pp.style.display==='block')api('/api/mem',function(r){r.json().then(function(j){rm.textContent='RAM int '+j.i+'K big '+j.l+'K min '+j.m+'K\nPSRAM '+j.p+'K'})})}
setInterval(memq,3000);
gg.addEventListener('click',memq);
var d0=null;
cv.addEventListener('pointerdown',function(e){d0={x:e.clientX,y:e.clientY}});
cv.addEventListener('pointerup',function(e){
 if(!T||!d0)return;
 if(Math.abs(e.clientX-d0.x)>12||Math.abs(e.clientY-d0.y)>12){d0=null;return}
 d0=null;
 var r=cv.getBoundingClientRect(),s=Math.min(r.width/cv.width,r.height/cv.height),ox=(r.width-cv.width*s)/2,oy=(r.height-cv.height*s)/2;
 var x=Math.floor((e.clientX-r.left-ox)/s),y=Math.floor((e.clientY-r.top-oy)/s);
 if(x<0||y<0||x>=cv.width||y>=cv.height)return;
 api('/api/tap?x='+x+'&y='+y);
});
auth();
var dr=null,acc=0,lastSend=0,pend=0;
function sendScroll(){if(pend!==0&&T){var n=pend;pend=0;api('/api/scroll?n='+n)}}
cv.addEventListener('pointerdown',function(e){dr={y:e.clientY};acc=0;try{cv.setPointerCapture(e.pointerId)}catch(_){}});
cv.addEventListener('pointermove',function(e){
 if(!dr||!T)return;
 var r=cv.getBoundingClientRect(),s=Math.min(r.width/cv.width,r.height/cv.height);
 acc+=(dr.y-e.clientY)/s;dr.y=e.clientY;
 var st=Math.trunc(acc/10);
 if(st!==0){acc-=st*10;pend+=st;var n=Date.now();if(n-lastSend>70){lastSend=n;sendScroll()}}
});
cv.addEventListener('pointerup',function(){dr=null;sendScroll()});
cv.addEventListener('pointercancel',function(){dr=null;sendScroll()});
cv.addEventListener('wheel',function(e){
 e.preventDefault();if(!T)return;
 pend+=(e.deltaY>0?1:-1);
 var n=Date.now();if(n-lastSend>40){lastSend=n;sendScroll()}else setTimeout(sendScroll,45);
},{passive:false});[].forEach.call(document.querySelectorAll('[data-l]'),function(b){b.onclick=function(){var l=b.getAttribute('data-l');api('/api/lang?l='+l,function(){tip('язык: '+l,3000)})}});
var JX=0,JY=0,JB=0,JM=localStorage.k85j==='1',jon=false,jlast=0;
function setJoy(on){if(on===jon)return;jon=on;document.getElementById('jy').style.display=on?'block':'none';if(on)tip('Дрон: стик внизу, шестерёнка слева сверху = меню',5000)}
function joySend(f){var n=Date.now();if(!f&&n-jlast<55)return;jlast=n;if(T)api('/api/joy?x='+Math.round(JX)+'&y='+Math.round(JY)+'&b='+JB)}
var js=document.getElementById('js'),jk=document.getElementById('jk'),sp=null;
function stickMove(e){var r=js.getBoundingClientRect(),R=r.width/2,dx=e.clientX-(r.left+R),dy=e.clientY-(r.top+R),l=Math.sqrt(dx*dx+dy*dy);if(l>R){dx=dx*R/l;dy=dy*R/l}jk.style.left=(R-30+dx)+'px';jk.style.top=(R-30+dy)+'px';JX=dx/R*100;JY=dy/R*100;joySend(0)}
js.addEventListener('pointerdown',function(e){sp=e.pointerId;try{js.setPointerCapture(sp)}catch(_){}stickMove(e)});
js.addEventListener('pointermove',function(e){if(e.pointerId===sp)stickMove(e)});
function stickEnd(e){if(e.pointerId!==sp)return;sp=null;JX=0;JY=0;jk.style.left='45px';jk.style.top='45px';joySend(1)}
js.addEventListener('pointerup',stickEnd);js.addEventListener('pointercancel',stickEnd);
var AR={u:0,d:0,l:0,r:0};
function arrCalc(){JX=(AR.r-AR.l)*100;JY=(AR.d-AR.u)*100;joySend(1)}
[].forEach.call(document.querySelectorAll('#ja b'),function(b){var k=b.getAttribute('data-d');
 b.addEventListener('pointerdown',function(e){AR[k]=1;try{b.setPointerCapture(e.pointerId)}catch(_){}arrCalc()});
 var up=function(){AR[k]=0;arrCalc()};b.addEventListener('pointerup',up);b.addEventListener('pointercancel',up)});
var jbEl=document.getElementById('jb');
jbEl.addEventListener('pointerdown',function(e){JB=1;try{jbEl.setPointerCapture(e.pointerId)}catch(_){}joySend(1)});
var jbUp=function(){JB=0;joySend(1)};
jbEl.addEventListener('pointerup',jbUp);jbEl.addEventListener('pointercancel',jbUp);
function jmode(){document.getElementById('js').style.display=JM?'none':'block';document.getElementById('ja').style.display=JM?'block':'none'}
document.getElementById('jm').onclick=function(){JM=!JM;localStorage.k85j=JM?'1':'0';JX=0;JY=0;joySend(1);jmode()};
setInterval(function(){if(jon&&T&&(JX||JY||JB))joySend(1)},70);
jmode();
</script></body></html>)HTML";

static void h_auth(int fd, const char *q, int64_t now) {
    char n[24];
    if (!qparam(q, "n", n, sizeof(n)) || strlen(n) < 8 || strlen(n) > 16) { send_err(fd, "400 Bad Request", "bad id"); return; }
    for (const char *c = n; *c; c++) {
        if (!isalnum((unsigned char)*c)) { send_err(fd, "400 Bad Request", "bad id"); return; }
    }
    char out[96];
    if (s_sess) {
        if (hl_ct_eq(n, s_sess_nonce)) {
            s_sess_last = now;
            snprintf(out, sizeof(out), "{\"s\":\"ok\",\"t\":\"%s\"}", s_tok);
            send_json(fd, out);
        } else {
            send_json(fd, "{\"s\":\"busy\"}");
        }
        return;
    }
    if (s_pend && !hl_ct_eq(n, s_pend_nonce)) { send_json(fd, "{\"s\":\"busy\"}"); return; }
    if (!s_pend) {
        snprintf(s_pend_nonce, sizeof(s_pend_nonce), "%s", n);
        s_pend = true; s_pend_since = now; s_both_since = 0;
        beep(1500, 80);
        ESP_LOGI(TAG, "login request, waiting for A+B on the stick");
    }
    snprintf(out, sizeof(out), "{\"s\":\"wait\",\"left\":%d}", (int)((60000000 - (now - s_pend_since)) / 1000000));
    send_json(fd, out);
}

// h_frame never blocks: answers 204 at once when there is nothing new (the page asks again in ~60 ms)
static void h_frame(int fd, const char *q) {
    char vs[16];
    uint32_t v = qparam(q, "v", vs, sizeof(vs)) ? (uint32_t)strtoul(vs, nullptr, 10) : 0;
    const size_t rb = (size_t)s_nw * 2;
    const size_t fbn = rb * (size_t)s_nh;
    int rot = (int)M5.Display.getRotation();
    bool force = (v == 0 || !s_prev_valid || v != s_prev_hash || rot != s_prev_rot);
    uint32_t wr = g_fb_wr;
    char ex[200];
    int64_t t = esp_timer_get_time();
    if (!force && (wr == s_seen_wr || t - s_last_frame < s_gap)) {
        snprintf(ex, sizeof(ex), "X-V: %u\r\n", (unsigned)s_prev_hash);
        send_resp(fd, "204 No Content", "text/plain", "", 0, ex);
        return;
    }
    s_seen_wr = wr;
    int y0 = 0, y1 = s_nh - 1;
    if (!force) {
        y0 = -1;
        for (int y = 0; y < s_nh; y++) {
            if (memcmp(s_fb_mem + (size_t)y * rb, s_prev + (size_t)y * rb, rb) != 0) { if (y0 < 0) y0 = y; y1 = y; }
        }
        if (y0 < 0) {
            snprintf(ex, sizeof(ex), "X-V: %u\r\n", (unsigned)s_prev_hash);
            send_resp(fd, "204 No Content", "text/plain", "", 0, ex);
            return;
        }
    }
    size_t off = (size_t)y0 * rb;
    size_t bytes = (size_t)(y1 - y0 + 1) * rb;
    memcpy(s_prev + off, s_fb_mem + off, bytes);          // from now on s_prev is exactly what the browser gets
    s_prev_rot = rot;
    s_prev_valid = true;
    s_prev_hash = fnv(s_prev, fbn);
    s_last_frame = esp_timer_get_time();

    size_t rl = rle_encode(s_prev + off, bytes / 2, s_rle, bytes);
    bool use = rl && rl < bytes;
    s_gap = ((use ? rl : bytes) > 24000) ? 130000 : 70000;   // 14 fps for small updates, ~8 fps for big frames
    snprintf(ex, sizeof(ex), "X-V: %u\r\nX-W: %d\r\nX-H: %d\r\nX-R: %d\r\nX-E: %d\r\nX-Y0: %d\r\nX-Y1: %d\r\n",
             (unsigned)s_prev_hash, s_nw, s_nh, rot, use ? 1 : 0, y0, y1);
    if (use) send_resp(fd, "200 OK", "application/octet-stream", s_rle, rl, ex);
    else send_resp(fd, "200 OK", "application/octet-stream", s_prev + off, bytes, ex);
}

static long content_length(const char *req, int hdr_end) {
    for (int i = 0; i + 15 < hdr_end; i++) {
        if ((req[i] | 32) == 'c' && strncasecmp(req + i, "content-length:", 15) == 0) return strtol(req + i + 15, nullptr, 10);
    }
    return -1;
}

// POST /api/up?name=dir/file.ext : raw body -> /littlefs/<name> (max 3 MB, ASCII names, 96 KB stays free)
static void h_upload(int fd, const char *q, const char *req, int len, int hdr_end) {
    s_keep = false;
    char enc[160], name[128];
    if (!qparam(q, "name", enc, sizeof(enc))) { send_err(fd, "400 Bad Request", "no name"); return; }
    url_decode(enc, name, sizeof(name));
    long cl = content_length(req, hdr_end);
    if (cl <= 0) { send_err(fd, "400 Bad Request", "empty file"); return; }
    if (cl > 3L * 1024 * 1024) { send_err(fd, "413 Payload Too Large", "max 3 MB"); return; }

    size_t nl = strlen(name);
    bool ok = nl > 0 && nl < 100 && name[0] != '/' && strstr(name, "..") == nullptr;
    for (size_t i = 0; ok && i < nl; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == ' ' || c == '/')) ok = false;
    }
    char full[160];
    if (ok) { snprintf(full, sizeof(full), "/littlefs/%s", name); ok = !k85_is_blocked_path(full); }
    if (!ok) { send_err(fd, "400 Bad Request", "bad name"); return; }

    size_t total = 0, used = 0;
    if (!k85_fs_info(&total, &used) || total < used || (size_t)cl + 96 * 1024 > total - used) { send_err(fd, "507 Insufficient Storage", "no space"); return; }

    for (char *p = full + 10; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(full, 0755); *p = '/'; }
    }
    char part[170];
    snprintf(part, sizeof(part), "%s.part", full);
    FILE *f = fopen(part, "wb");
    if (!f) { send_err(fd, "500 Internal Server Error", "cannot create file"); return; }

    static EXT_RAM_BSS_ATTR uint8_t buf[4096];
    long got = 0;
    bool wok = true;
    int body_have = len - (hdr_end + 4);
    if (body_have > 0) {
        long n = body_have > cl ? cl : body_have;
        if (fwrite(req + hdr_end + 4, 1, (size_t)n, f) != (size_t)n) wok = false;
        got += n;
    }
    int64_t dl = esp_timer_get_time() + 120LL * 1000000;
    while (wok && got < cl) {
        if (esp_timer_get_time() > dl) { wok = false; break; }
        long left = cl - got;
        int want = left > (long)sizeof(buf) ? (int)sizeof(buf) : (int)left;
        int r = recv(fd, buf, want, 0);
        if (r <= 0) { wok = false; break; }
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { wok = false; break; }
        got += r;
        s_sess_last = esp_timer_get_time();
    }
    bool closed = (fclose(f) == 0);
    if (!wok || got != cl || !closed) {
        remove(part);
        send_err(fd, "500 Internal Server Error", "upload failed");
        return;
    }
    remove(full);
    if (rename(part, full) != 0) { remove(part); send_err(fd, "500 Internal Server Error", "rename failed"); return; }
    char out[64];
    snprintf(out, sizeof(out), "{\"s\":\"ok\",\"n\":%ld}", got);
    send_json(fd, out);
    ESP_LOGI(TAG, "uploaded %s (%ld B)", name, got);
}

// ---- music for the phone: file list + HTTP range streaming (the phone plays it into its own headphones) ----
static bool rel_ok(const char *s) {
    if (s[0] == '/' || strstr(s, "..") != nullptr || strlen(s) >= 110) return false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '\\') return false;
    }
    return true;
}
static void map_path(const char *rel, char *out, size_t n) {
    if (!strcmp(rel, "sd")) snprintf(out, n, "/sd");
    else if (!strncmp(rel, "sd/", 3)) snprintf(out, n, "/sd/%s", rel + 3);
    else if (rel[0]) snprintf(out, n, "/littlefs/%s", rel);
    else snprintf(out, n, "/littlefs");
}
static bool audio_ext(const char *name, const char **mime, bool *wav) {
    size_t l = strlen(name);
    *wav = false;
    if (l > 4 && !strcasecmp(name + l - 4, ".mp3")) { *mime = "audio/mpeg"; return true; }
    if (l > 4 && !strcasecmp(name + l - 4, ".wav")) { *mime = "audio/wav"; *wav = true; return true; }
    return false;
}

static void h_ls(int fd, const char *q) {
    char enc[140], rel[120], base[160];
    rel[0] = 0;
    if (qparam(q, "dir", enc, sizeof(enc))) url_decode(enc, rel, sizeof(rel));
    if (!rel_ok(rel)) { send_err(fd, "400 Bad Request", "bad dir"); return; }
    map_path(rel, base, sizeof(base));
    static EXT_RAM_BSS_ATTR char js[6144];
    size_t o = 0;
    o += snprintf(js + o, sizeof(js) - o, "{\"e\":[");
    int cnt = 0;
    DIR *d = opendir(base);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr && cnt < 60 && o + 160 < sizeof(js)) {
            const char *nm = e->d_name;
            if (!nm[0] || nm[0] == '.' || strlen(nm) > 60) continue;
            char fp[240];
            snprintf(fp, sizeof(fp), "%s/%s", base, nm);
            struct stat st;
            if (stat(fp, &st) != 0 || k85_is_blocked_path(fp)) continue;
            const char *mime = nullptr;
            bool wav = false;
            bool isdir = S_ISDIR(st.st_mode);
            if (!isdir && !audio_ext(nm, &mime, &wav)) continue;
            if (cnt) js[o++] = ',';
            o += snprintf(js + o, sizeof(js) - o, "{\"k\":%d,\"n\":\"", isdir ? 1 : 0);
            for (const char *c = nm; *c && o + 8 < sizeof(js); c++) {
                if (*c == '"' || *c == '\\') js[o++] = '\\';
                if ((unsigned char)*c >= 0x20) js[o++] = *c;
            }
            o += snprintf(js + o, sizeof(js) - o, "\"}");
            cnt++;
        }
        closedir(d);
    }
    if (!rel[0]) {
        DIR *sd = opendir("/sd");
        if (sd) {
            closedir(sd);
            if (cnt < 60) o += snprintf(js + o, sizeof(js) - o, "%s{\"k\":1,\"n\":\"sd\"}", cnt ? "," : "");
        }
    }
    o += snprintf(js + o, sizeof(js) - o, "]}");
    send_resp(fd, "200 OK", "application/json", js, o, nullptr);
}

static void h_audio(int fd, const char *q, const char *req, int hdr_end) {
    s_keep = false;
    char enc[200], rel[140], base[200];
    if (!qparam(q, "p", enc, sizeof(enc))) { send_err(fd, "400 Bad Request", "no file"); return; }
    url_decode(enc, rel, sizeof(rel));
    const char *mime = nullptr;
    bool wav = false;
    if (!rel_ok(rel) || !audio_ext(rel, &mime, &wav)) { send_err(fd, "400 Bad Request", "bad file"); return; }
    map_path(rel, base, sizeof(base));
    if (k85_is_blocked_path(base)) { send_err(fd, "403 Forbidden", "blocked"); return; }
    struct stat st;
    if (stat(base, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size <= 0) { send_err(fd, "404 Not Found", "no file"); return; }
    long total = (long)st.st_size;

    long s = 0, e = -1;
    for (int i = 0; i + 6 < hdr_end; i++) {
        if ((req[i] | 32) == 'r' && strncasecmp(req + i, "range:", 6) == 0) {
            const char *p = strstr(req + i, "bytes=");
            if (p && p < req + hdr_end) {
                char *ep = nullptr;
                s = strtol(p + 6, &ep, 10);
                if (ep && *ep == '-') {
                    ep++;
                    if (*ep >= '0' && *ep <= '9') e = strtol(ep, nullptr, 10);
                }
            }
            break;
        }
    }
    if (s < 0) { s = total + s; if (s < 0) s = 0; }          // "bytes=-N" = the last N bytes
    if (s >= total) { send_err(fd, "416 Range Not Satisfiable", "range"); return; }
    const long slice = wav ? 160 * 1024 : 64 * 1024;          // short pieces: the screen stream keeps running between them
    if (e < 0 || e >= total || e < s) e = total - 1;  // K85FIX4: "bytes=100-50" gave a negative length
    if (e - s + 1 > slice) e = s + slice - 1;
    long n = e - s + 1;

    FILE *f = fopen(base, "rb");
    if (!f) { send_err(fd, "404 Not Found", "open failed"); return; }
    fseek(f, s, SEEK_SET);
    char h[360];
    int hn = snprintf(h, sizeof(h), "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nContent-Length: %ld\r\nAccept-Ranges: bytes\r\nContent-Range: bytes %ld-%ld/%ld\r\nConnection: close\r\n\r\n",
                      mime, n, s, e, total);
    if (send_all(fd, h, (size_t)hn)) {
        static EXT_RAM_BSS_ATTR uint8_t ab[4096];
        long left = n;
        while (left > 0) {
            size_t want = left > (long)sizeof(ab) ? sizeof(ab) : (size_t)left;
            size_t r = fread(ab, 1, want, f);
            if (r == 0) break;
            if (!send_all(fd, ab, r)) break;
            left -= (long)r;
        }
    }
    fclose(f);
}

// returns true when the connection stays open for the next request
static bool handle(int fd) {
    static EXT_RAM_BSS_ATTR char req[1800];
    int64_t t0 = esp_timer_get_time();
    int len = 0;
    char *he = nullptr;
    while (len < (int)sizeof(req) - 1) {
        int r = recv(fd, req + len, sizeof(req) - 1 - len, 0);
        if (r <= 0) return false;                 // closed by the browser, or it went silent
        len += r;
        req[len] = 0;
        he = strstr(req, "\r\n\r\n");
        if (he) break;
    }
    if (!he) return false;
    int hdr_end = (int)(he - req);
    s_keep = true;
    for (int i = 0; i + 11 < hdr_end; i++) {
        if ((req[i] | 32) == 'c' && strncasecmp(req + i, "connection:", 11) == 0) {
            const char *p = req + i + 11;
            while (*p == ' ') p++;
            if (!strncasecmp(p, "close", 5)) s_keep = false;
            break;
        }
    }

    char method[8] = {0}, full[300] = {0};
    sscanf(req, "%7s %299s", method, full);
    char *qm = strchr(full, '?');
    const char *q = "";
    if (qm) { *qm = 0; q = qm + 1; }
    int64_t now = esp_timer_get_time();

    if (!strcmp(full, "/")) {
        send_resp(fd, "200 OK", "text/html; charset=utf-8", PAGE, strlen(PAGE), nullptr);
    } else if (!strcmp(full, "/api/auth")) {
        h_auth(fd, q, now);
    } else if (!authed(q, now)) {
        send_err(fd, "401 Unauthorized", "login");
    } else if (!strcmp(full, "/api/f")) {
        h_frame(fd, q);
    } else if (!strcmp(full, "/api/up") && !strcmp(method, "POST")) {
        h_upload(fd, q, req, len, hdr_end);
    } else if (!strcmp(full, "/api/ls")) {
        h_ls(fd, q);
    } else if (!strcmp(full, "/api/audio")) {
        h_audio(fd, q, req, hdr_end);
    } else if (!strcmp(full, "/api/joy")) {
        char jx[8], jy[8], jb[4];
        if (qparam(q, "x", jx, sizeof(jx)) && qparam(q, "y", jy, sizeof(jy)) && qparam(q, "b", jb, sizeof(jb))) k85_joy_post(atoi(jx), atoi(jy), atoi(jb));
        send_json(fd, "{\"s\":\"ok\"}");
    } else if (!strcmp(full, "/api/scroll")) {
        char ns[8];
        if (qparam(q, "n", ns, sizeof(ns))) k85_scroll_post(atoi(ns));
        send_json(fd, "{\"s\":\"ok\"}");
    } else if (!strcmp(full, "/api/lang")) {
        char lv[4];
        if (qparam(q, "l", lv, sizeof(lv))) k85_lang_set(!strcmp(lv, "ru"));
        send_json(fd, "{\"s\":\"ok\"}");
    } else if (!strcmp(full, "/api/tap")) {
        char xs[8], ys[8];
        if (qparam(q, "x", xs, sizeof(xs)) && qparam(q, "y", ys, sizeof(ys))) k85_pointer_post(atoi(xs), atoi(ys));
        send_json(fd, "{\"s\":\"ok\"}");
    } else if (!strcmp(full, "/api/mem")) {
        char mj[120];
        snprintf(mj, sizeof(mj), "{\"i\":%u,\"l\":%u,\"m\":%u,\"p\":%u}",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        send_json(fd, mj);
    } else if (!strcmp(full, "/api/back")) {
        send_json(fd, "{\"s\":\"ok\"}");
        pin_pulse(800);
    } else if (!strcmp(full, "/api/bios")) {
        s_bios_req = true;
        send_json(fd, "{\"s\":\"ok\"}");
    } else if (!strcmp(full, "/api/mode")) {
        char m[8];
        if (qparam(q, "m", m, sizeof(m)) && (!strcmp(m, "on") || !strcmp(m, "off"))) {
            k85_svc_set_str("headless", m);
            send_json(fd, "{\"s\":\"ok\"}");
        } else {
            send_err(fd, "400 Bad Request", "bad mode");
        }
    } else if (!strcmp(full, "/api/ap")) {   // K85WPA3
        char apw[200], apd[72];
        if (!qparam(q, "p", apw, sizeof(apw))) apw[0] = 0;
        url_decode(apw, apd, sizeof(apd));
        const size_t apn = strlen(apd);
        bool apok = (apn == 0) || (apn >= 8 && apn <= 63);
        for (size_t i = 0; apok && i < apn; i++) {
            const unsigned char ch = (unsigned char)apd[i];
            if (ch < 0x20 || ch > 0x7E) apok = false;
        }
        if (apok) {
            k85_svc_set_str("hl_pass", apn ? apd : nullptr);
            send_json(fd, "{\"s\":\"ok\"}");
        } else {
            send_err(fd, "400 Bad Request", "bad password");
        }
    } else if (!strcmp(full, "/api/reboot")) {
        s_keep = false;
        send_json(fd, "{\"s\":\"ok\"}");
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
    } else {
        send_err(fd, "404 Not Found", "not found");
    }
    int64_t dt = esp_timer_get_time() - t0;
    if (dt > 400000) ESP_LOGW(TAG, "slow request %s %d ms", full, (int)(dt / 1000));
    return s_keep;
}

// ---------------- AP + watchdog ----------------
// K85WPA3: the AP password (set with "headless pass" or from the web page) lives in k85_svc.cfg, which is closed to the shell and FTP.
// A+B at power-on opens the AP for that boot. true = the AP is protected (WPA2/WPA3 mixed mode).
static bool hl_ap_pass(char *out, size_t n) {
    out[0] = 0;
    if (s_pw_override) return false;
    if (!k85_svc_get_str("hl_pass", out, n)) { out[0] = 0; return false; }
    const size_t l = strlen(out);
    if (l < 8 || l > 63) { out[0] = 0; return false; }
    return true;
}

static void ap_config_apply(void) {
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "k85OS-%02X%02X", mac[4], mac[5]);
    int channel = g_config.ap_channel;
    if (channel < 1 || channel > 13) channel = 1;
    wifi_config_t ap = {};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", ssid);
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    char k85_pw[66];
    if (hl_ap_pass(k85_pw, sizeof(k85_pw))) {   // K85WPA3
        snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", k85_pw);
        ap.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        ap.ap.pmf_cfg.capable = true;
        ap.ap.pmf_cfg.required = false;
        ap.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap.ap.max_connection = 3;
    ap.ap.channel = (uint8_t)channel;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_set_ps(WIFI_PS_NONE);                // lowest latency for the browser
}

// BLE tools stop the whole WiFi driver to free RAM; bring the AP back when they are done
static void ap_watchdog(void) {
    static int64_t next = 0;
    int64_t now = esp_timer_get_time();
    if (now < next) return;
    next = now + 2000000;
    wifi_mode_t m = WIFI_MODE_NULL;
    esp_err_t e = esp_wifi_get_mode(&m);
    if (e == ESP_OK && (m & WIFI_MODE_AP)) return;                 // the AP is up
    if (e == ESP_ERR_WIFI_NOT_INIT) {
        if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) return;   // a BLE tool is running and owns the RAM
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 12 * 1024 || heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 4 * 1024) return;
        k85_wifi_set_low_mem(true);
        k85_wifi_init();
        ESP_LOGW(TAG, "WiFi was stopped (BLE tool?): restarting the AP");
    } else if (e != ESP_OK) {
        return;
    }
    ap_config_apply();
}

// ---------------- server: one thread, several open connections (keep-alive), nobody waits for a silent one ----------------
static void hl_task(void *arg) {
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) { ESP_LOGE(TAG, "socket failed"); vTaskDelete(nullptr); return; }
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(80);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 6) != 0) {
        ESP_LOGE(TAG, "bind/listen failed");
        close(ls);
        vTaskDelete(nullptr);
        return;
    }
    int pool[HL_POOL];
    int64_t last[HL_POOL];
    for (int i = 0; i < HL_POOL; i++) { pool[i] = -1; last[i] = 0; }

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ls, &fds);
        int mx = ls;
        for (int i = 0; i < HL_POOL; i++) {
            if (pool[i] >= 0) { FD_SET(pool[i], &fds); if (pool[i] > mx) mx = pool[i]; }
        }
        struct timeval tv = {0, 20000};
        int sel = select(mx + 1, &fds, nullptr, nullptr, &tv);
        int64_t now = esp_timer_get_time();
        tick(now);
        ap_watchdog();
        int fresh = -1;
        if (sel > 0) {
            if (FD_ISSET(ls, &fds)) {
                int c = accept(ls, nullptr, nullptr);
                if (c >= 0) {
                    int slot = -1;
                    for (int i = 0; i < HL_POOL; i++) if (pool[i] < 0) { slot = i; break; }
                    if (slot < 0) {                               // full: drop the connection that was idle the longest
                        slot = 0;
                        for (int i = 1; i < HL_POOL; i++) if (last[i] < last[slot]) slot = i;
                        close(pool[slot]);
                        ESP_LOGW(TAG, "pool full, dropped an old connection");
                    }
                    struct timeval t1 = {1, 0};
                    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &t1, sizeof(t1));
                    struct timeval t2 = {3, 0};
                    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &t2, sizeof(t2));
                    int one = 1;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    pool[slot] = c;
                    last[slot] = now;
                    fresh = slot;
                } else {
                    ESP_LOGW(TAG, "accept failed, errno %d", errno);
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
            for (int i = 0; i < HL_POOL; i++) {
                if (i == fresh || pool[i] < 0 || !FD_ISSET(pool[i], &fds)) continue;
                if (handle(pool[i])) {
                    last[i] = esp_timer_get_time();
                } else {
                    close(pool[i]);
                    pool[i] = -1;
                }
            }
        }
        now = esp_timer_get_time();
        for (int i = 0; i < HL_POOL; i++) {
            if (pool[i] >= 0 && now - last[i] > 8000000) { close(pool[i]); pool[i] = -1; }
        }
    }
}

void k85_headless_start(void) {
    if (!s_active || s_started) return;
    k85_voice_dl_abort();
    k85_wifi_init();
    unsigned fi = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    unsigned lb = (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    ESP_LOGI(TAG, "internal RAM: free %u KB, largest block %u KB", fi, lb);
    if (fi < 8 || lb < 3) { ESP_LOGE(TAG, "not enough internal RAM for the AP"); return; }

    ap_config_apply();
    mdns_init();
    mdns_hostname_set("k85os");
    mdns_instance_name_set("k85OS");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);

    if (!s_stack) s_stack = (StackType_t *)heap_caps_malloc(HL_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_stack) { ESP_LOGE(TAG, "no stack for the web task"); return; }
    xTaskCreateStaticPinnedToCore(hl_task, "k85_hl", HL_STACK, nullptr, 6, s_stack, &s_tcb, 1);   // core 1, above the UI task
    s_started = true;
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    { char k85_pwx[66]; ESP_LOGI(TAG, "web UI: WiFi k85OS-%02X%02X (%s), http://192.168.4.1 or http://k85os.local", mac[4], mac[5], hl_ap_pass(k85_pwx, sizeof(k85_pwx)) ? "WPA2/WPA3" : "open"); }
}

void k85_headless_cmd(const char *arg, char *out, size_t out_size) {
    while (*arg == ' ') arg++;
    if (!strncmp(arg, "pass", 4) && (arg[4] == ' ' || arg[4] == 0)) {   // K85WPA3
        const char *pw = arg + 4;
        while (*pw == ' ') pw++;
        char cur[66];
        if (!*pw) {
            snprintf(out, out_size, "AP password: %s\r\nusage: headless pass <8-63 chars> | headless pass off\r\n", hl_ap_pass(cur, sizeof(cur)) ? "set (WPA2/WPA3)" : "none (open AP)");
            return;
        }
        if (!strcmp(pw, "off")) {
            k85_svc_set_str("hl_pass", nullptr);
            snprintf(out, out_size, "AP password removed (open AP after reboot)\r\n");
            return;
        }
        const size_t pn = strlen(pw);
        if (pn < 8 || pn > 63) { snprintf(out, out_size, "headless pass: 8..63 characters\r\n"); return; }
        k85_svc_set_str("hl_pass", pw);
        snprintf(out, out_size, "AP password set: WPA2/WPA3 after reboot\r\n");
        return;
    }
    if (!strcmp(arg, "on") || !strcmp(arg, "off")) k85_svc_set_str("headless", arg);
    int m = hl_mode();
    snprintf(out, out_size, "headless: %s, mode %s (changes apply after reboot)\r\nusage: headless on|off\r\n",
             s_active ? "ACTIVE" : "inactive", m == 0 ? "off" : "on");
}