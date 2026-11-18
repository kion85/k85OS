// K85WSCAN1: WiFi analyzer. Channel load bars + AP list + radar sweep.
// A = next view (both / channels / list), B = radar and back, hold A+B (1.2 s) = exit.
#include "wifi_scanner.h"
#include "wifi.h"
#include "theme.h"
#include "input.h"
#include "power.h"
#include "headless.h"

#include "M5Unified.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#define WS_NCH      13
#define WS_MAXAP    40
#define WS_RECS     24
#define WS_EXIT_US  1200000LL
#define WS_PI       3.14159265f

typedef lgfx::LovyanGFX Gfx;

struct WsAp { char ssid[33]; int8_t rssi; uint8_t ch; };

static EXT_RAM_BSS_ATTR WsAp s_ap[WS_MAXAP];
static int s_nap = 0;
static int64_t s_ch_time[WS_NCH + 1];
static float s_load[WS_NCH + 1];
static EXT_RAM_BSS_ATTR wifi_ap_record_t s_recs[WS_RECS];
static volatile bool s_scan_done = false;

static void ws_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_SCAN_DONE) s_scan_done = true;
}

static uint32_t ws_rssi_col(int rssi) { return rssi > -60 ? 0x22CC55 : (rssi > -75 ? 0xFFCC00 : 0xFF6666); }
static uint32_t ws_load_col(float l) { return l < 0.8f ? 0x22CC55 : (l < 1.8f ? 0xFFCC00 : 0xFF3333); }

// replaces everything known about channel ch with the fresh scan result
static void ws_collect(int ch) {
    uint16_t n = WS_RECS;
    if (esp_wifi_scan_get_ap_records(&n, s_recs) != ESP_OK) n = 0;
    int w = 0;
    for (int i = 0; i < s_nap; i++) if (s_ap[i].ch != ch) s_ap[w++] = s_ap[i];
    s_nap = w;
    for (int i = 0; i < n; i++) {
        if (s_recs[i].primary != ch) continue;
        WsAp a;
        memset(&a, 0, sizeof(a));
        snprintf(a.ssid, sizeof(a.ssid), "%s", (const char *)s_recs[i].ssid);
        a.rssi = s_recs[i].rssi;
        a.ch = (uint8_t)ch;
        if (s_nap < WS_MAXAP) {
            s_ap[s_nap++] = a;
        } else {
            int wk = 0;
            for (int k = 1; k < s_nap; k++) if (s_ap[k].rssi < s_ap[wk].rssi) wk = k;
            if (a.rssi > s_ap[wk].rssi) s_ap[wk] = a;
        }
    }
    s_ch_time[ch] = esp_timer_get_time();
}

// channel load: every AP loads its own channel fully and the neighbours (+-1, +-2) partly
static void ws_compute(int *busy, int *freech) {
    for (int c = 1; c <= WS_NCH; c++) s_load[c] = 0;
    for (int i = 0; i < s_nap; i++) {
        float w = (s_ap[i].rssi + 100) / 70.0f;
        if (w < 0) w = 0;
        if (w > 1) w = 1;
        for (int c = 1; c <= WS_NCH; c++) {
            int d = abs(c - (int)s_ap[i].ch);
            float k = d == 0 ? 1.0f : (d == 1 ? 0.6f : (d == 2 ? 0.3f : 0.0f));
            s_load[c] += w * k;
        }
    }
    int b = 1, f = 1;
    for (int c = 2; c <= WS_NCH; c++) {
        if (s_load[c] > s_load[b]) b = c;
        if (s_load[c] < s_load[f]) f = c;
    }
    *busy = b;
    *freech = f;
}

static void ws_draw_channels(Gfx &g, int x, int y, int w, int h, int cur, int busy, int freech,
                             uint32_t fg, uint32_t bg, uint32_t accent, int err) {
    g.fillRect(x, y, w, h, bg);
    g.setTextSize(1);
    g.setTextColor(accent, bg);
    g.setCursor(x + 2, y);
    if (s_nap > 0) g.printf("Busy:%d Free:%d", busy, freech);
    else if (err) g.printf("scan err %x", err);
    else g.print("Scanning...");
    int top = y + 12, bot = y + h - 11;
    if (bot - top < 10) return;
    int bw = w / WS_NCH;
    if (bw < 3) bw = 3;
    float maxl = 2.5f;
    for (int c = 1; c <= WS_NCH; c++) if (s_load[c] > maxl) maxl = s_load[c];
    for (int c = 1; c <= WS_NCH; c++) {
        int bx = x + (c - 1) * bw;
        int bh = (int)((bot - top) * s_load[c] / maxl);
        if (bh < 1 && s_load[c] > 0) bh = 1;
        if (bh > bot - top) bh = bot - top;
        if (bh > 0) g.fillRect(bx + 1, bot - bh, bw - 2, bh, ws_load_col(s_load[c]));
        if (s_nap > 0 && c == busy) g.drawRect(bx, top, bw, bot - top, 0xFF3333);
        if (c == cur) g.fillTriangle(bx + bw / 2 - 3, bot + 1, bx + bw / 2 + 3, bot + 1, bx + bw / 2, bot + 4, accent);
        if (c % 2 == 1) {
            int digits = c >= 10 ? 2 : 1;
            g.setTextColor(c == cur ? accent : fg, bg);
            g.setCursor(bx + bw / 2 - digits * 3, bot + 3);
            g.printf("%d", c);
        }
    }
}

static void ws_draw_list(Gfx &g, int x, int y, int w, int h, uint32_t fg, uint32_t bg, uint32_t accent) {
    (void)fg;
    g.fillRect(x, y, w, h, bg);
    g.setTextSize(1);
    int rows = (h - 12) / 9;
    if (rows < 1) rows = 1;
    int idx[WS_MAXAP];
    for (int i = 0; i < s_nap; i++) idx[i] = i;
    for (int i = 1; i < s_nap; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && s_ap[idx[j]].rssi < s_ap[v].rssi) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    int pages = (s_nap + rows - 1) / rows;
    if (pages < 1) pages = 1;
    int page = (int)((esp_timer_get_time() / 3000000LL) % pages);
    g.setTextColor(accent, bg);
    g.setCursor(x + 2, y);
    g.printf("WiFi %d  %d/%d", s_nap, page + 1, pages);
    int namew = (w / 6) - 9;
    if (namew < 4) namew = 4;
    if (namew > 32) namew = 32;
    for (int r = 0; r < rows; r++) {
        int k = page * rows + r;
        if (k >= s_nap) break;
        const WsAp &a = s_ap[idx[k]];
        char line[56];
        snprintf(line, sizeof(line), "%-*.*s %3d %2d", namew, namew, a.ssid[0] ? a.ssid : "<hidden>", (int)a.rssi, (int)a.ch);
        g.setTextColor(ws_rssi_col(a.rssi), bg);
        g.setCursor(x + 2, y + 12 + r * 9);
        g.print(line);
    }
}

static void ws_draw_radar(Gfx &g, int x, int y, int w, int h, int cur, float frac, int busy, int freech,
                          uint32_t fg, uint32_t bg, uint32_t accent) {
    (void)fg;
    g.fillRect(x, y, w, h, bg);
    int d = (w < h) ? w : h;
    int r = d / 2 - 4;
    if (r < 10) return;
    bool wide = (w >= h + 70);
    int cx = wide ? (x + r + 6) : (x + w / 2);
    int cy = y + h / 2;
    g.drawCircle(cx, cy, r, 0x2E7D32);
    g.drawCircle(cx, cy, r * 2 / 3, 0x1B5E20);
    g.drawCircle(cx, cy, r / 3, 0x1B5E20);
    g.drawFastHLine(cx - r, cy, 2 * r + 1, 0x1B5E20);
    g.drawFastVLine(cx, cy - r, 2 * r + 1, 0x1B5E20);

    float base = ((cur - 1) + frac) * 2.0f * WS_PI / WS_NCH - WS_PI / 2;
    for (int t = 7; t >= 0; t--) {
        float a = base - t * 0.07f;
        int gv = 255 - t * 32;
        g.drawLine(cx, cy, cx + (int)(cosf(a) * r), cy + (int)(sinf(a) * r), (uint32_t)gv << 8);
    }

    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_nap; i++) {
        const WsAp &a = s_ap[i];
        float ang = ((a.ch - 1) + 0.5f) * 2.0f * WS_PI / WS_NCH - WS_PI / 2;
        float q = (a.rssi + 95) / 65.0f;
        if (q < 0) q = 0;
        if (q > 1) q = 1;
        unsigned hsh = (unsigned)(a.ssid[0] * 31 + a.ssid[1] * 7 + a.rssi);
        ang += ((int)(hsh % 7) - 3) * 0.025f;
        float dist = r * (0.92f - 0.77f * q) * (1.0f - ((hsh / 7) % 5) * 0.03f);
        int px = cx + (int)(cosf(ang) * dist);
        int py = cy + (int)(sinf(ang) * dist);
        int pr = 2 + (int)(q * 2);
        bool fresh = (now - s_ch_time[a.ch]) < 700000;
        g.fillCircle(px, py, pr, fresh ? 0xFFFFFF : ws_rssi_col(a.rssi));
        if (fresh) g.drawCircle(px, py, pr + 2, accent);
    }

    int top = -1;
    for (int i = 0; i < s_nap; i++) if (top < 0 || s_ap[i].rssi > s_ap[top].rssi) top = i;
    g.setTextSize(1);
    g.setTextColor(accent, bg);
    if (wide) {
        int tx = cx + r + 10;
        g.setCursor(tx, y + 4);  g.print("RADAR");
        g.setTextColor(0xFFFFFF, bg);
        g.setCursor(tx, y + 18); g.printf("APs: %d", s_nap);
        g.setCursor(tx, y + 30); g.printf("Busy ch: %d", busy);
        g.setCursor(tx, y + 42); g.printf("Free ch: %d", freech);
        g.setCursor(tx, y + 54); g.printf("Scan ch: %d", cur);
        if (top >= 0) {
            g.setTextColor(ws_rssi_col(s_ap[top].rssi), bg);
            g.setCursor(tx, y + 70); g.printf("%.12s", s_ap[top].ssid[0] ? s_ap[top].ssid : "<hidden>");
            g.setCursor(tx, y + 82); g.printf("%d dBm ch%d", (int)s_ap[top].rssi, (int)s_ap[top].ch);
        }
    } else {
        g.setCursor(x + 2, y);
        g.printf("APs:%d B:%d F:%d", s_nap, busy, freech);
    }
}

void k85_run_wifi_scanner(void) {
    k85_wifi_init();
    s_nap = 0;
    memset(s_ch_time, 0, sizeof(s_ch_time));
    memset(s_load, 0, sizeof(s_load));
    s_scan_done = false;

    esp_event_handler_instance_t inst = nullptr;
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &ws_evt, nullptr, &inst);

    const int W = M5.Display.width();
    const int H = M5.Display.height();
    M5Canvas cv(&M5.Display);
    cv.setPsram(true);
    bool have_cv = cv.createSprite(W, H) != nullptr;
    Gfx &g = have_cv ? (Gfx &)cv : (Gfx &)M5.Display;

    const uint32_t bg = k85_get_bg();
    const uint32_t fg = k85_get_fg();
    const uint32_t accent = k85_get_accent();

    int mode = 0, prev_mode = 0;          // 0 both, 1 channels, 2 list, 3 radar
    int scan_ch = 0;
    bool scanning = false;
    int64_t scan_t0 = 0, next_try = 0, last_draw = 0;
    int64_t pend_a = 0, pend_b = 0, hold0 = 0;
    bool chord = false;
    int last_err = 0;

    while (true) {
        k85_input_update();
        int64_t now = esp_timer_get_time();
        bool ad = k85_btn_a_is_down(), bd = k85_btn_b_is_down();
        if (k85_btn_a_pressed()) { pend_a = now; k85_wake_screen(); }
        if (k85_btn_b_pressed()) { pend_b = now; k85_wake_screen(); }

        if (ad && bd) {
            if (!hold0) hold0 = now;
            chord = true;
            pend_a = pend_b = 0;
        } else {
            hold0 = 0;
            if (!ad && !bd) chord = false;
        }
        if (hold0 && (now - hold0) >= WS_EXIT_US) break;

        // a single press is applied after 220 ms, so the first button of an A+B chord does nothing
        if (pend_a && (now - pend_a) > 220000) {
            if (!chord) mode = (mode == 3) ? 0 : (mode + 1) % 3;
            pend_a = 0;
        }
        if (pend_b && (now - pend_b) > 220000) {
            if (!chord) {
                if (mode == 3) mode = prev_mode; else { prev_mode = mode; mode = 3; }
            }
            pend_b = 0;
        }

        // channel by channel scan (non blocking)
        if (!scanning && now >= next_try) {
            int nc = scan_ch % WS_NCH + 1;
            wifi_scan_config_t sc = {};
            sc.channel = (uint8_t)nc;
            sc.show_hidden = true;
            sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            sc.scan_time.active.min = k85_headless_active() ? 30 : 70;
            sc.scan_time.active.max = k85_headless_active() ? 60 : 130;
            s_scan_done = false;
            esp_err_t e = esp_wifi_scan_start(&sc, false);
            if (e == ESP_OK) { scanning = true; scan_ch = nc; scan_t0 = now; last_err = 0; }
            else { last_err = (int)e; next_try = now + 300000; }
        } else if (scanning) {
            if (s_scan_done) {
                ws_collect(scan_ch); next_try = now + (k85_headless_active() ? 350000 : 0);
                scanning = false;
            } else if (now - scan_t0 > 2000000) {
                esp_wifi_scan_stop();
                scanning = false;
                next_try = now + 200000;
            }
        }

        if (now - last_draw >= (k85_headless_active() ? 140000 : 70000)) {
            last_draw = now;
            int busy = 1, freech = 1;
            ws_compute(&busy, &freech);
            int cur = scan_ch ? scan_ch : 1;
            float frac = 1.0f;
            if (scanning) { frac = (now - scan_t0) / 200000.0f; if (frac > 1) frac = 1; }
            int bh = H - 10;
            g.fillRect(0, 0, W, H, bg);
            if (mode == 0) {
                if (W >= H) {
                    ws_draw_channels(g, 0, 0, W / 2 - 1, bh, cur, busy, freech, fg, bg, accent, last_err);
                    ws_draw_list(g, W / 2, 0, W - W / 2, bh, fg, bg, accent);
                } else {
                    ws_draw_channels(g, 0, 0, W, bh / 2, cur, busy, freech, fg, bg, accent, last_err);
                    ws_draw_list(g, 0, bh / 2, W, bh - bh / 2, fg, bg, accent);
                }
            } else if (mode == 1) {
                ws_draw_channels(g, 0, 0, W, bh, cur, busy, freech, fg, bg, accent, last_err);
            } else if (mode == 2) {
                ws_draw_list(g, 0, 0, W, bh, fg, bg, accent);
            } else {
                ws_draw_radar(g, 0, 0, W, bh, cur, frac, busy, freech, fg, bg, accent);
            }
            g.setTextSize(1);
            if (hold0) {
                int pw = (int)((int64_t)W * (now - hold0) / WS_EXIT_US);
                if (pw > W) pw = W;
                g.fillRect(0, H - 9, pw, 9, 0xFF3333);
                g.setTextColor(0xFFFFFF);
                g.setCursor(2, H - 8);
                g.print("hold A+B to exit");
            } else {
                g.setTextColor(0x888888, bg);
                g.setCursor(2, H - 8);
                g.print(W >= 200 ? "A=view B=radar hold A+B=exit" : "A=view B=radar");
            }
            if (have_cv) cv.pushSprite(0, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    if (scanning) esp_wifi_scan_stop();
    esp_wifi_clear_ap_list();
    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, inst);
    if (have_cv) cv.deleteSprite();
    k85_wait_ab_release();
}