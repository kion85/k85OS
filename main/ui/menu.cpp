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
#include "../apps/tools/json_interpreter.h"
#include "rtc_ntp.h"
#include "clock_menu.h"
#include "../apps/wifi_menu.h"
#include "../apps/apps_menu.h"
#include "wifi.h"
#include "M5Unified.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>
#include <cmath>

static const char *const K85_MENU_ITEMS[] = {
    "Low tone", "High tone", "Both tones", "Cube", "Colors",
    "Clock", "WiFi", "Apps", "Tools", "Games", "Interpreter", "Settings", "System info", "Logs", "Notifications",
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
        if (balanced && !strcmp(item, "Cube")) continue;
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
        float t = ((float)y / (float)h) * 0.35f;
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

static void draw_menu_icon(int cx, int cy, int r, const char *name, uint32_t col) {
    auto &d = M5.Display;
    if (!strcmp(name, "Low tone") || !strcmp(name, "High tone") || !strcmp(name, "Both tones")) {
        d.fillRect(cx - r, cy - r/2, r/2, r, col);
        d.fillTriangle(cx - r/2, cy - r, cx - r/2, cy + r, cx + r/4, cy, col);
        int waves = !strcmp(name, "Low tone") ? 1 : (!strcmp(name, "High tone") ? 2 : 3);
        for (int i = 0; i < waves; i++) {
            int off = r/2 + i * 3;
            d.drawLine(cx + r/3, cy - off/2, cx + r/3 + off/2, cy - off, col);
            d.drawLine(cx + r/3, cy + off/2, cx + r/3 + off/2, cy + off, col);
        }
    } else if (!strcmp(name, "Cube")) {
        int s = r;
        d.drawRect(cx - s, cy - s/2, s, s, col);
        d.drawRect(cx - s + s/3, cy - s/2 - s/3, s, s, col);
        d.drawLine(cx - s, cy - s/2, cx - s + s/3, cy - s/2 - s/3, col);
        d.drawLine(cx, cy - s/2, cx + s/3, cy - s/2 - s/3, col);
        d.drawLine(cx - s, cy + s/2, cx - s + s/3, cy + s/2 - s/3, col);
        d.drawLine(cx, cy + s/2, cx + s/3, cy + s/2 - s/3, col);
    } else if (!strcmp(name, "Colors")) {
        d.fillCircle(cx - r/2, cy - r/3, r/3, 0xFF0000);
        d.fillCircle(cx + r/2, cy - r/3, r/3, 0x00FF00);
        d.fillCircle(cx, cy + r/3, r/3, 0x0000FF);
    } else if (!strcmp(name, "Clock")) {
        d.drawCircle(cx, cy, r, col);
        d.drawLine(cx, cy, cx, cy - r + 2, col);
        d.drawLine(cx, cy, cx + r/2, cy, col);
    } else if (!strcmp(name, "WiFi")) {
        d.fillRect(cx - r/2, cy + r/2 - 2, 3, 3, col);
        d.fillRect(cx - r/6, cy + r/4 - 2, 3, r/2, col);
        d.fillRect(cx + r/6, cy - 2, 3, r - 2, col);
    } else if (!strcmp(name, "Apps")) {
        int s = r/2 - 1;
        d.fillRect(cx - r/2, cy - r/2, s, s, col);
        d.fillRect(cx + 2, cy - r/2, s, s, col);
        d.fillRect(cx - r/2, cy + 2, s, s, col);
        d.fillRect(cx + 2, cy + 2, s, s, col);
    } else if (!strcmp(name, "Tools")) {
        d.fillCircle(cx - r/2, cy - r/2, r/3, col);
        d.drawLine(cx - r/2, cy - r/2, cx + r/2, cy + r/2, col);
        d.drawLine(cx - r/2 + 1, cy - r/2, cx + r/2 + 1, cy + r/2, col);
    } else if (!strcmp(name, "Games")) {
        d.fillRoundRect(cx - r, cy - r/2, r * 2, r, r/3, col);
        d.fillCircle(cx + r/2, cy, 2, 0x000000);
        d.fillCircle(cx + r/2 + 5, cy - 3, 2, 0x000000);
    } else if (!strcmp(name, "Settings")) {
        d.fillCircle(cx, cy, r/2, col);
        for (int a = 0; a < 360; a += 45) {
            float rad = a * 3.14159f / 180.0f;
            int x1 = cx + (int)(cosf(rad) * (r/2));
            int y1 = cy + (int)(sinf(rad) * (r/2));
            int x2 = cx + (int)(cosf(rad) * r);
            int y2 = cy + (int)(sinf(rad) * r);
            d.drawLine(x1, y1, x2, y2, col);
        }
    } else if (!strcmp(name, "System info")) {
        d.drawCircle(cx, cy, r, col);
        d.fillRect(cx - 1, cy - r/3, 2, r/3, col);
        d.fillRect(cx - 1, cy - r/2 - 2, 2, 2, col);
    } else if (!strcmp(name, "Logs")) {
        d.drawFastHLine(cx - r, cy - r/2, r * 2 - r/3, col);
        d.drawFastHLine(cx - r, cy, r * 2, col);
        d.drawFastHLine(cx - r, cy + r/2, r * 2 - r/2, col);
    } else if (!strcmp(name, "Notifications")) {
        d.fillCircle(cx, cy - 2, r/2, col);
        d.fillRect(cx - r/4, cy + r/3, r/2, 2, col);
        d.drawCircle(cx, cy - r - 1, 2, col);
    } else if (!strcmp(name, "Interpreter")) {
        d.drawLine(cx - r/2, cy - r/2, cx - r/3, cy - r/4, col);
        d.drawLine(cx - r/3, cy - r/4, cx - r/3, cy + r/4, col);
        d.drawLine(cx - r/3, cy + r/4, cx - r/2, cy + r/2, col);
        d.drawLine(cx + r/2, cy - r/2, cx + r/3, cy - r/4, col);
        d.drawLine(cx + r/3, cy - r/4, cx + r/3, cy + r/4, col);
        d.drawLine(cx + r/3, cy + r/4, cx + r/2, cy + r/2, col);
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
}

static void draw_menu_grid(void);
static void draw_menu_list_icons(void);

static void draw_menu_list(void) {
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
        k85_status_bar_draw();
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
    k85_status_bar_draw();
}

void k85_menu_draw(void) {
    switch (g_config.menu_ui_style) {
        case 1: draw_menu_grid(); break;
        case 2: draw_menu_list_icons(); break;
        default: draw_menu_list(); break;
    }
}
static void draw_menu_list_icons(void) {
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
        k85_status_bar_draw();
        return;
    }
    if (s_selected >= count) s_selected = count - 1;
    if (s_selected < 0) s_selected = 0;

    draw_menu_background(bg, accent);

    const int line_h = 22;
    const int start_y = 16;
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

    M5.Display.setTextSize(1);
    for (int i = s_scroll_offset; i < end_index; i++) {
        int yy = start_y + (i - s_scroll_offset) * line_h;
        bool sel = (i == s_selected);
        if (sel) {
            M5.Display.fillRoundRect(2, yy - 2, w - 4, 16, 4, accent);
        }
        uint32_t item_fg = sel ? 0x000000 : fg;
        uint32_t item_bg = sel ? accent : bg;
        draw_menu_icon(14, yy + 6, 8, items[i], item_fg);
        M5.Display.setTextColor(item_fg, item_bg);
        M5.Display.setCursor(26, yy + 2);
        M5.Display.print(items[i]);
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    if (s_scroll_offset > 0) {
        M5.Display.setCursor(w - 14, 4);
        M5.Display.print("^");
    }
    if (end_index < count) {
        M5.Display.setCursor(w - 14, h - 12);
        M5.Display.print("v");
    }
    k85_status_bar_draw();
}
static void draw_menu_grid(void) {
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
        k85_status_bar_draw();
        return;
    }
    if (s_selected >= count) s_selected = count - 1;
    if (s_selected < 0) s_selected = 0;

    draw_menu_background(bg, accent);

    const int cols = 4;
    const int rows = 2;
    const int per_page = cols * rows;
    const int start_y = 16;
    int grid_h = h - start_y - 10;
    int cell_w = w / cols;
    int cell_h = grid_h / rows;

    int page = s_selected / per_page;
    int page_count = (count + per_page - 1) / per_page;
    int page_start = page * per_page;
    int page_end = page_start + per_page;
    if (page_end > count) page_end = count;

    for (int i = page_start; i < page_end; i++) {
        int local = i - page_start;
        int col = local % cols;
        int row = local / cols;
        int cx = col * cell_w + cell_w / 2;
        int cy = start_y + row * cell_h + cell_h / 2 - 6;

        bool sel = (i == s_selected);
        if (sel) {
            M5.Display.fillRoundRect(col * cell_w + 3, start_y + row * cell_h + 2,
                                      cell_w - 6, cell_h - 4, 6, accent);
        }

        draw_menu_icon(cx, cy, 12, items[i], sel ? 0x000000 : fg);

        M5.Display.setTextSize(1);
        M5.Display.setTextColor(sel ? 0x000000 : fg, sel ? accent : bg);
        char short_label[16];
        int max_chars = (cell_w - 4) / 6;
        if (max_chars > 15) max_chars = 15;
        if (max_chars < 1) max_chars = 1;
        snprintf(short_label, sizeof(short_label), "%.*s", max_chars, items[i]);
        int tx = col * cell_w + (cell_w - (int)strlen(short_label) * 6) / 2;
        if (tx < col * cell_w) tx = col * cell_w + 1;
        M5.Display.setCursor(tx, start_y + row * cell_h + cell_h - 12);
        M5.Display.print(short_label);
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    if (page_count > 1) {
        char pg[32];
        snprintf(pg, sizeof(pg), "%d/%d", page + 1, page_count);
        M5.Display.setCursor(w - (int)strlen(pg) * 6 - 4, h - 10);
        M5.Display.print(pg);
    }

    k85_status_bar_draw();
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
    } else if (!strcmp(item, "Apps")) {
        k85_run_apps_menu();
    } else if (!strcmp(item, "Games")) {
        k85_run_games_menu();
    } else if (!strcmp(item, "Colors")) {
        run_colors();
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
        if (!strcmp(item, "Interpreter")) { k85_run_json_interpreter(); } else { run_placeholder(item); }
    }
}

void k85_menu_activate(void) {
    run_action(s_selected);
    k85_menu_draw();
}