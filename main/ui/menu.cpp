#include "menu.h"
#include "common.h"
#include "theme.h"
#include "battery.h"
#include "../core/status_bar.h"
#include "power.h"
#include "input.h"
#include "sound.h"
#include "log.h"
#include "config.h"
#include "notifications.h"
#include "system_info.h"
#include "logs_screen.h"
#include "settings_menu.h"
#include "store.h"
#include "tools_menu.h"
#include "games_menu.h"
#include "rtc_ntp.h"
#include "clock_menu.h"
#include "../apps/wifi_menu.h"
#include "wifi.h"
#include "M5Unified.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>

static const char *const K85_MENU_ITEMS[] = {
    "Low tone", "High tone", "Both tones", "Cube", "Colors",
    "Clock", "WiFi", "Store", "Tools", "Games", "Settings", "System info", "Logs", "Notifications",
};
#define K85_MENU_ITEM_COUNT (int)(sizeof(K85_MENU_ITEMS) / sizeof(K85_MENU_ITEMS[0]))

static int s_selected = 0;
static int s_scroll_offset = 0;

static int get_filtered_menu(const char *out[], int max_out) {
    int mode_idx = g_config.battery_mode_idx;
    const char *mode = (mode_idx >= 0 && mode_idx < K85_BATTERY_MODE_COUNT)
                            ? k85_battery_modes[mode_idx] : "Balanced";
    int n = 0;

    if (!strcmp(mode, "SuperEco")) {
        if (max_out > 0) out[n++] = "Settings";
        if (max_out > 1) out[n++] = "System info";
        return n;
    }

    bool balanced = !strcmp(mode, "Balanced");
    for (int i = 0; i < K85_MENU_ITEM_COUNT && n < max_out; i++) {
        const char *item = K85_MENU_ITEMS[i];
        if (balanced && (!strcmp(item, "Cube") || !strcmp(item, "Store"))) continue;
        out[n++] = item;
    }
    return n;
}

static void draw_menu_background(uint32_t bg, uint32_t accent) {
    if (!g_config.bg_gradient_enabled) {
        M5.Display.fillScreen(bg);
        return;
    }
    int h = M5.Display.height();
    int w = M5.Display.width();
    int tr = (bg >> 16) & 0xFF, tg = (bg >> 8) & 0xFF, tb = bg & 0xFF;
    int br = (accent >> 16) & 0xFF, bg2 = (accent >> 8) & 0xFF, bb = accent & 0xFF;
    for (int y = 0; y < h; y++) {
        float t = ((float)y / (float)h) * 0.35f; // мягкий градиент, не перебивает читаемость текста
        int r = tr + (int)((br - tr) * t);
        int g = tg + (int)((bg2 - tg) * t);
        int b = tb + (int)((bb - tb) * t);
        M5.Display.drawFastHLine(0, y, w, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
    }
}

void k85_menu_init(void) {
    s_selected = 0;
    s_scroll_offset = 0;
}

void k85_menu_draw(void) {
    const char *items[K85_MENU_ITEM_COUNT];
    int count = get_filtered_menu(items, K85_MENU_ITEM_COUNT);

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int w = M5.Display.width();
    int h = M5.Display.height();

    if (count == 0) {
        draw_menu_background(bg, accent);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(10, h / 2 - 8);
        M5.Display.print("No items");
        k85_draw_battery_icon();
        return;
    }

    if (s_selected >= count) s_selected = count - 1;
    if (s_selected < 0) s_selected = 0;

    draw_menu_background(bg, accent);
    M5.Display.setTextSize(2);
    const int line_h = 26;
    const int start_y = 14;
    int visible_count = (h - start_y) / line_h;
    if (visible_count < 1) visible_count = 1;

    if (s_selected < s_scroll_offset) {
        s_scroll_offset = s_selected;
    } else if (s_selected >= s_scroll_offset + visible_count) {
        s_scroll_offset = s_selected - visible_count + 1;
    }

    int max_scroll = count - visible_count;
    if (max_scroll < 0) max_scroll = 0;
    if (s_scroll_offset > max_scroll) s_scroll_offset = max_scroll;
    if (s_scroll_offset < 0) s_scroll_offset = 0;

    int end_index = s_scroll_offset + visible_count;
    if (end_index > count) end_index = count;

    for (int i = s_scroll_offset; i < end_index; i++) {
        int yy = start_y + (i - s_scroll_offset) * line_h;
        if (i == s_selected) {
            M5.Display.setTextColor(0x000000, accent);
            M5.Display.setCursor(4, yy);
            M5.Display.printf(">%s", items[i]);
        } else {
            M5.Display.setTextColor(fg, bg);
            M5.Display.setCursor(4, yy);
            M5.Display.printf(" %s", items[i]);
        }
    }

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xAAAAAA, bg);
    if (s_scroll_offset > 0) {
        M5.Display.setCursor(w - 14, 4);
        M5.Display.print("^");
    }
    if (end_index < count) {
        M5.Display.setCursor(w - 14, h - 12);
        M5.Display.print("v");
    }

    k85_draw_battery_icon();
}

void k85_menu_next(void) {
    const char *items[K85_MENU_ITEM_COUNT];
    int count = get_filtered_menu(items, K85_MENU_ITEM_COUNT);
    if (count > 0) {
        s_selected = (s_selected + 1) % count;
    }
    k85_menu_draw();
}

static bool play_tone_blocking(uint32_t freq, uint32_t dur_ms) {
    k85_play_tone(freq, dur_ms);
    for (uint32_t elapsed = 0; elapsed < dur_ms; elapsed += 100) {
        k85_input_update();
        if (k85_ab_held(300)) {
            k85_wait_ab_release();
            k85_speaker_stop();
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    k85_speaker_stop();
    return false;
}

// ---------- Colors ----------
struct K85ColorEntry {
    const char *name;
    uint32_t val;
};

static const K85ColorEntry K85_COLORS[] = {
    {"RED",   0xFF0000},
    {"GREEN", 0x00FF00},
    {"BLUE",  0x0000FF},
    {"WHITE", 0xFFFFFF},
};
#define K85_COLOR_COUNT (int)(sizeof(K85_COLORS) / sizeof(K85_COLORS[0]))

static void draw_color_screen(int idx) {
    uint32_t val = K85_COLORS[idx].val;
    // На белом фоне текст чёрный, на остальных — белый (как в оригинале)
    uint32_t txt_col = (val == 0xFFFFFF) ? 0x000000 : 0xFFFFFF;

    M5.Display.fillScreen(val);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(txt_col, val);
    M5.Display.setCursor(10, M5.Display.height() / 2 - 8);
    M5.Display.print(K85_COLORS[idx].name);
}

static void run_colors(void) {
    int idx = 0;
    draw_color_screen(idx);

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }

        if (k85_btn_a_pressed()) {
            idx = (idx + 1) % K85_COLOR_COUNT;
            draw_color_screen(idx);
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Cube ----------
#include <cmath>

struct K85Point3D { float x, y, z; };

static const K85Point3D K85_CUBE_PTS[8] = {
    {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
    {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},
};

static const int K85_CUBE_EDGES[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

static K85Point3D cube_rotate(const K85Point3D &p, float ax, float ay) {
    float y2 = p.y * cosf(ax) - p.z * sinf(ax);
    float z2 = p.y * sinf(ax) + p.z * cosf(ax);
    float x3 = p.x * cosf(ay) + z2 * sinf(ay);
    float z3 = -p.x * sinf(ay) + z2 * cosf(ay);
    return {x3, y2, z3};
}

static void cube_project(const K85Point3D &p, float scale, int cx, int cy, int &out_x, int &out_y) {
    const float fov = 4.0f;
    float f = fov / (fov + p.z);
    out_x = cx + (int)(p.x * scale * f);
    out_y = cy + (int)(p.y * scale * f);
}

static void run_cube(void) {
    M5.Display.fillScreen(k85_get_bg());
    k85_draw_battery_icon();

    float angle_x = 0.0f;
    float angle_y = 0.0f;
    int cx = M5.Display.width() / 2;
    int cy = M5.Display.height() / 2;
    float scale = 30.0f;
    const int frame_delay_ms = 40;

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }

        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        M5.Imu.getAccel(&ax, &ay, &az);

        int px[8], py[8];
        for (int i = 0; i < 8; i++) {
            K85Point3D rp = cube_rotate(K85_CUBE_PTS[i], angle_x, angle_y);
            cube_project(rp, scale, cx, cy, px[i], py[i]);
        }

        uint32_t bg = k85_get_bg();
        M5.Display.fillScreen(bg);
        for (int i = 0; i < 12; i++) {
            int a = K85_CUBE_EDGES[i][0];
            int b = K85_CUBE_EDGES[i][1];
            M5.Display.drawLine(px[a], py[a], px[b], py[b], 0x00FFFF);
        }
        k85_draw_battery_icon();

        angle_x += 0.02f + ay * 0.08f;
        angle_y += 0.015f + ax * 0.08f;

        vTaskDelay(pdMS_TO_TICKS(frame_delay_ms));
    }
}
// ---------- Clock ----------
static void run_clock(void) {
    if (!k85_is_ntp_synced()) {
        if (k85_wifi_is_connected()) {
            k85_ntp_sync_now();
        }
    }

    uint32_t bg = k85_get_bg();
    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(k85_get_accent(), bg);
    M5.Display.setCursor(10, 6);
    M5.Display.printf("Clock [%s]", k85_is_ntp_synced() ? "NTP" : "NO NTP");

    int scr_w = M5.Display.width();
    int scr_h = M5.Display.height();
    uint32_t elapsed_ms = 500; // форсируем первую отрисовку сразу

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }

        if (elapsed_ms >= 500) {
            elapsed_ms = 0;

            const char *t = k85_get_time_str();
            M5.Display.setTextSize(3);
            M5.Display.setTextColor(k85_get_fg(), k85_get_bg());
            int approx_w = (int)strlen(t) * 18;
            M5.Display.setCursor((scr_w - approx_w) / 2, scr_h / 2 - 16);
            M5.Display.print(t);

            if (k85_is_ntp_synced()) {
                const char *d = k85_get_date_str();
                M5.Display.setTextSize(1);
                M5.Display.setTextColor(0x888888, k85_get_bg());
                M5.Display.setCursor((scr_w - (int)strlen(d) * 6) / 2, scr_h / 2 + 8);
                M5.Display.print(d);
            }

            k85_draw_battery_icon();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed_ms += 50;
    }
}
static void run_placeholder(const char *name) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s\n(not implemented yet)\nA+B=back", name);
    k85_show_message(buf);
    k85_log("Placeholder opened: %s", name);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void run_action(int index) {
    const char *items[K85_MENU_ITEM_COUNT];
    int count = get_filtered_menu(items, K85_MENU_ITEM_COUNT);
    if (index < 0 || index >= count) return;
    const char *item = items[index];

    if (!strcmp(item, "Low tone")) {
        k85_show_message("Low tone\nA+B=stop");
        play_tone_blocking(200, 3000);
    } else if (!strcmp(item, "High tone")) {
        k85_show_message("High tone\nA+B=stop");
        play_tone_blocking(800, 3000);
    } else if (!strcmp(item, "Both tones")) {
        k85_show_message("Low...\nA+B=stop");
        if (!play_tone_blocking(200, 1500)) {
            k85_show_message("High...\nA+B=stop");
            play_tone_blocking(800, 1500);
        }
    } else if (!strcmp(item, "Cube")) {
        run_cube();
    } else if (!strcmp(item, "Clock")) {
        k85_run_clock_menu();
    } else if (!strcmp(item, "WiFi")) {
        k85_run_wifi_menu();
    } else if (!strcmp(item, "Games")) {
        k85_run_games_menu();
    } else if (!strcmp(item, "Colors")) {
        run_colors();
    } else if (!strcmp(item, "Store")) {
        k85_run_store();
    } else if (!strcmp(item, "Tools")) {
        k85_run_tools_menu();
    } else if (!strcmp(item, "System info")) {
        k85_run_system_info();
    } else if (!strcmp(item, "Logs")) {
        k85_run_logs_screen();
    } else if (!strcmp(item, "Notifications")) {
        k85_run_notifications_screen();
    } else if (!strcmp(item, "Settings")) {
        k85_run_settings_menu();
    } else {
        run_placeholder(item);
    }
}

void k85_menu_activate(void) {
    run_action(s_selected);
    k85_menu_draw();
}













