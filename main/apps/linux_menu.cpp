#include "linux_menu.h"
#include "list_menu.h"
#include "theme.h"
#include "input.h"
#include "common.h"
#include "battery.h"
#include "wifi.h"
#include "config.h"
#include "device.h"
#include "rtc_ntp.h"
#include "version.h"
#include "M5Unified.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

void k85_run_task_manager(void);   // apps/tools/task_manager.cpp  ("top")

static void wait_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

// ============================ neofetch ============================
static void draw_tux(int ox, int oy) {
    auto &d = M5.Display;
    d.fillEllipse(ox + 20, oy + 30, 15, 17, 0x555555);            // body
    d.fillEllipse(ox + 20, oy + 33, 9, 12, 0xFFFFFF);             // belly
    d.fillCircle(ox + 20, oy + 13, 10, 0x555555);                 // head
    d.fillCircle(ox + 16, oy + 11, 3, 0xFFFFFF);                  // eyes
    d.fillCircle(ox + 24, oy + 11, 3, 0xFFFFFF);
    d.fillCircle(ox + 16, oy + 11, 1, 0x000000);
    d.fillCircle(ox + 24, oy + 11, 1, 0x000000);
    d.fillTriangle(ox + 15, oy + 16, ox + 25, oy + 16, ox + 20, oy + 22, 0xFFC000);   // beak
    d.fillEllipse(ox + 11, oy + 46, 7, 3, 0xFF8000);              // feet
    d.fillEllipse(ox + 29, oy + 46, 7, 3, 0xFF8000);
}

static void draw_neofetch(void) {
    auto &d = M5.Display;
    uint32_t bg = k85_get_bg(), fg = k85_get_fg(), accent = k85_get_accent();
    int w = d.width();
    bool wide = w >= 200;
    d.fillScreen(bg);
    int tx, ty;
    if (wide) { draw_tux(6, 8); tx = 56; ty = 6; }
    else      { draw_tux((w - 40) / 2, 4); tx = 6; ty = 56; }
    d.setTextSize(1);

    int y = ty;
    auto kv = [&](const char *k, const char *v) {
        d.setCursor(tx, y);
        d.setTextColor(accent, bg);
        d.print(k);
        d.setTextColor(fg, bg);
        d.print(v);
        y += 10;
    };

    char head[40];
    snprintf(head, sizeof(head), "%.12s@k85os", k85_get_device_name());
    d.setCursor(tx, y); d.setTextColor(accent, bg); d.print(head); y += 10;
    d.setCursor(tx, y); d.setTextColor(fg, bg); d.print("---------------"); y += 10;

    char v[48];
    snprintf(v, sizeof(v), "k85OS v%s", K85_FW_VERSION);            kv("OS: ", v);
    kv("Kernel: ", "FreeRTOS");

    uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
    if (sec >= 3600) snprintf(v, sizeof(v), "%uh %02um %02us", (unsigned)(sec / 3600), (unsigned)((sec / 60) % 60), (unsigned)(sec % 60));
    else             snprintf(v, sizeof(v), "%um %02us", (unsigned)(sec / 60), (unsigned)(sec % 60));
    kv("Uptime: ", v);

    snprintf(v, sizeof(v), "ESP32-S3 %uMHz", (unsigned)esp_rom_get_cpu_ticks_per_us());
    kv("CPU: ", v);

    size_t it = heap_caps_get_total_size(MALLOC_CAP_INTERNAL), ifr = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(v, sizeof(v), "%uK/%uK", (unsigned)((it - ifr) / 1024), (unsigned)(it / 1024));
    kv("RAM: ", v);

    size_t pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM), pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (pt) snprintf(v, sizeof(v), "%uK/%uK", (unsigned)((pt - pf) / 1024), (unsigned)(pt / 1024));
    else    snprintf(v, sizeof(v), "none");
    kv("PSRAM: ", v);

    uint32_t flash = 0;
    esp_flash_get_physical_size(nullptr, &flash);
    snprintf(v, sizeof(v), "%u MB", (unsigned)(flash / (1024 * 1024)));
    kv("Flash: ", v);

    int b = k85_get_battery();
    if (b >= 0) snprintf(v, sizeof(v), "%d%%", b); else snprintf(v, sizeof(v), "n/a");
    kv("Battery: ", v);

    if (k85_wifi_is_connected()) snprintf(v, sizeof(v), "%.12s", g_config.wifi_ssid); else snprintf(v, sizeof(v), "off");
    kv("WiFi: ", v);

    // colour blocks like the real neofetch
    static const uint32_t pal[8] = {0x000000, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
    int px = wide ? 6 : tx;
    int py = wide ? 64 : y + 4;
    for (int i = 0; i < 8; i++) {
        d.fillRect(px + i * 10, py, 9, 8, pal[i]);
        if (i == 0) d.drawRect(px, py, 9, 8, fg);
    }
}

static void run_neofetch(void) {
    k85_wait_ab_release();
    bool first = true;
    uint32_t last = 0;
    while (true) {
        k85_input_update();
        if (k85_btn_a_pressed() || k85_btn_b_pressed() || k85_ab_held(300)) { k85_wait_ab_release(); return; }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (first || now - last >= 1000) {
            first = false;
            last = now;
            draw_neofetch();
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

// ============================ cmatrix ============================
static void run_matrix(void) {
    auto &d = M5.Display;
    const int cw = 6, ch = 8;
    int w = d.width(), h = d.height();
    int cols = w / cw;
    if (cols > 48) cols = 48;
    int rows = h / ch;
    if (rows < 1) rows = 1;
    static int head[48], trail[48], speed[48], tick[48];

    d.fillScreen(0x000000);
    d.setTextSize(1);
    for (int c = 0; c < cols; c++) {
        head[c] = -(int)(esp_random() % (uint32_t)rows);
        trail[c] = 5 + (int)(esp_random() % 10);
        speed[c] = 1 + (int)(esp_random() % 3);
        tick[c] = 0;
    }
    k85_wait_ab_release();
    char s[2] = {0, 0};
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        for (int c = 0; c < cols; c++) {
            if (++tick[c] < speed[c]) continue;
            tick[c] = 0;
            head[c]++;
            int x = c * cw;
            if (head[c] >= 0 && head[c] < rows) {
                s[0] = (char)(33 + esp_random() % 94);
                d.setTextColor(0xCCFFCC, 0x000000);
                d.setCursor(x, head[c] * ch);
                d.print(s);
            }
            int p = head[c] - 1;
            if (p >= 0 && p < rows) {
                s[0] = (char)(33 + esp_random() % 94);
                d.setTextColor(0x00C040, 0x000000);
                d.setCursor(x, p * ch);
                d.print(s);
            }
            int e = head[c] - trail[c];
            if (e >= 0 && e < rows) d.fillRect(x, e * ch, cw, ch, 0x000000);
            if (e >= rows) {
                head[c] = -(int)(esp_random() % 8);
                trail[c] = 5 + (int)(esp_random() % 10);
                speed[c] = 1 + (int)(esp_random() % 3);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(35));
    }
}

// ============================ fortune | cowsay ============================
static const char *const FORTUNES[] = {
    "There is no place like 127.0.0.1.",
    "sudo make me a sandwich.",
    "It works on my machine.",
    "Keep it simple, stupid.",
    "rm -rf is forever.",
    "Have you tried turning it off and on again?",
    "Talk is cheap. Show me the code.",
    "Premature optimization is the root of all evil.",
    "Real programmers use butterflies.",
    "Don't panic.",
    "A watched build never finishes.",
    "Everything is a file.",
    "Release early, release often.",
    "Make it work, make it right, make it fast.",
};
#define FORTUNE_COUNT (int)(sizeof(FORTUNES) / sizeof(FORTUNES[0]))

static void draw_cowsay(const char *text) {
    auto &d = M5.Display;
    uint32_t bg = k85_get_bg(), fg = k85_get_fg(), accent = k85_get_accent();
    int w = d.width();
    int max_chars = (w - 12) / 6 - 4;
    if (max_chars > 28) max_chars = 28;
    if (max_chars < 10) max_chars = 10;

    char lines[5][32];
    int n = 0, widest = 0;
    int len = (int)strlen(text), pos = 0;
    while (pos < len && n < 5) {
        int take = len - pos;
        if (take > max_chars) {
            take = max_chars;
            int sp = take;
            while (sp > 0 && text[pos + sp] != ' ') sp--;
            if (sp > 0) take = sp;
        }
        if (take > 31) take = 31;
        memcpy(lines[n], text + pos, (size_t)take);
        lines[n][take] = 0;
        if (take > widest) widest = take;
        pos += take;
        while (pos < len && text[pos] == ' ') pos++;
        n++;
    }

    d.fillScreen(bg);
    d.setTextSize(1);
    d.setTextColor(fg, bg);
    int y = 6, x = 4;
    char bar[40], row[48];

    memset(bar, '_', (size_t)widest + 2); bar[widest + 2] = 0;
    snprintf(row, sizeof(row), " %s", bar);
    d.setCursor(x, y); d.print(row); y += 10;
    for (int i = 0; i < n; i++) {
        char l = '|', r = '|';
        if (n == 1) { l = '<'; r = '>'; }
        else if (i == 0) { l = '/'; r = '\\'; }
        else if (i == n - 1) { l = '\\'; r = '/'; }
        snprintf(row, sizeof(row), "%c %-*s %c", l, widest, lines[i], r);
        d.setCursor(x, y); d.print(row); y += 10;
    }
    memset(bar, '-', (size_t)widest + 2); bar[widest + 2] = 0;
    snprintf(row, sizeof(row), " %s", bar);
    d.setCursor(x, y); d.print(row); y += 10;

    d.setTextColor(accent, bg);
    static const char *const cow[5] = {
        "  \\  ^__^",
        "   \\ (oo)\\___",
        "     (__)\\   )\\/\\",
        "         ||--w |",
        "         ||    ||",
    };
    for (int i = 0; i < 5; i++) { d.setCursor(x, y); d.print(cow[i]); y += 10; }
}

static void run_fortune(void) {
    int idx = (int)(esp_random() % (uint32_t)FORTUNE_COUNT);
    bool redraw = true;
    k85_wait_ab_release();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed() || k85_btn_b_pressed()) {
            idx = (idx + 1 + (int)(esp_random() % (uint32_t)(FORTUNE_COUNT - 1))) % FORTUNE_COUNT;
            redraw = true;
        }
        if (redraw) { redraw = false; draw_cowsay(FORTUNES[idx]); }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

// ============================ cal ============================
static void run_cal(void) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) {
        k85_show_message("Time not set\nConnect WiFi (NTP)\nA+B=back");
        wait_exit();
        return;
    }
    static const char *const months[12] = {"January", "February", "March", "April", "May", "June",
                                           "July", "August", "September", "October", "November", "December"};
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int y = t.tm_year + 1900, m = t.tm_mon;
    int days = dim[m];
    if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days = 29;
    int first = ((t.tm_wday - (t.tm_mday - 1)) % 7 + 7) % 7;      // weekday of the 1st, 0 = Sunday

    auto &d = M5.Display;
    uint32_t bg = k85_get_bg(), fg = k85_get_fg(), accent = k85_get_accent();
    int w = d.width();
    d.fillScreen(bg);
    d.setTextSize(1);

    char title[32];
    snprintf(title, sizeof(title), "%s %d", months[m], y);
    d.setTextColor(accent, bg);
    d.setCursor((w - (int)strlen(title) * 6) / 2, 6);
    d.print(title);

    int x0 = (w - 120) / 2;
    static const char *const wd[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    d.setTextColor(0x888888, bg);
    for (int i = 0; i < 7; i++) { d.setCursor(x0 + i * 18, 24); d.print(wd[i]); }

    for (int day = 1; day <= days; day++) {
        int col = (first + day - 1) % 7, row = (first + day - 1) / 7;
        int x = x0 + col * 18, yy = 38 + row * 12;
        char num[16];
        snprintf(num, sizeof(num), "%2d", day);
        if (day == t.tm_mday) {
            d.fillRect(x - 2, yy - 2, 16, 11, accent);
            d.setTextColor(bg, accent);
        } else {
            d.setTextColor(fg, bg);
        }
        d.setCursor(x, yy);
        d.print(num);
    }
    k85_wait_ab_release();
    wait_exit();
}

// ============================ menu ============================
void k85_run_linux_menu(void) {
    static const char *const items[] = {"neofetch", "cmatrix", "fortune | cowsay", "cal", "top", "Back"};
    const int count = (int)(sizeof(items) / sizeof(items[0]));
    while (true) {
        int idx = k85_run_list_menu("LINUX", items, count, nullptr);
        if (idx < 0 || idx == count - 1) return;
        switch (idx) {
            case 0: run_neofetch(); break;
            case 1: run_matrix(); break;
            case 2: run_fortune(); break;
            case 3: run_cal(); break;
            case 4: k85_run_task_manager(); break;
            default: break;
        }
    }
}