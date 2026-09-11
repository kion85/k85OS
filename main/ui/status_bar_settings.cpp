#include "status_bar_settings.h"
#include "common.h"
#include "theme.h"
#include "input.h"
#include "../core/config.h"
#include "../core/status_bar.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

#define K85_SB_ITEM_COUNT 12
#define K85_SB_VISIBLE_ROWS 8

struct SbItem {
    const char *label;
    uint32_t bit; // 0 = спец-пункт (цвет фона)
};

static const SbItem k85_sb_items[K85_SB_ITEM_COUNT] = {
    {"Battery %",       SB_BIT_BATTERY_PCT},
    {"Battery: bolt icon", SB_BIT_BATTERY_BOLT},
    {"Time",            SB_BIT_TIME},
    {"Date",            SB_BIT_DATE},
    {"Uptime",          SB_BIT_UPTIME},
    {"WiFi status",     SB_BIT_WIFI},
    {"Bluetooth status",SB_BIT_BLUETOOTH},
    {"Sound volume",    SB_BIT_SOUND},
    {"Steps",           SB_BIT_STEPS},
    {"Free RAM %",      SB_BIT_RAM},
    {"Chip temp",       SB_BIT_TEMP},
    {"Background color",0},
};

static const uint32_t k85_sb_bg_presets[] = {
    0xFFFFFFFF, 0x000000, 0x1A1A2E, 0x330000, 0x003300, 0xFFFFFF,
};
#define K85_SB_BG_PRESET_COUNT (int)(sizeof(k85_sb_bg_presets) / sizeof(k85_sb_bg_presets[0]))

static int s_selected = 0;
static int s_scroll_top = 0;

static void sb_value_str(char *out, size_t out_size, int i) {
    if (k85_sb_items[i].bit != 0) {
        snprintf(out, out_size, "%s", (g_config.sb_flags & k85_sb_items[i].bit) ? "ON" : "off");
    } else {
        int idx = 0;
        for (int p = 0; p < K85_SB_BG_PRESET_COUNT; p++) {
            if (k85_sb_bg_presets[p] == g_config.sb_bg_color) { idx = p; break; }
        }
        snprintf(out, out_size, "%s", idx == 0 ? "theme" : "custom");
    }
}

// Иконки для Grid / List+Icons режимов.
static void draw_sb_icon(int cx, int cy, int r, const char *name, uint32_t col, uint32_t bg_col) {
    auto &d = M5.Display;
    if (!strcmp(name, "Battery %")) {
        d.drawRect(cx - r, cy - r/2, r * 2 - 2, r, col);
        d.fillRect(cx + r - 2, cy - r/4, 2, r/2, col);
        d.fillRect(cx - r + 2, cy - r/2 + 2, r, r - 4, col);
    } else if (!strcmp(name, "Battery: bolt icon")) {
        d.drawRect(cx - r, cy - r/2, r * 2 - 2, r, col);
        d.fillRect(cx + r - 2, cy - r/4, 2, r/2, col);
        d.fillTriangle(cx - 2, cy - r/2, cx - r/3, cy, cx, cy, bg_col);
        d.fillTriangle(cx, cy, cx - r/4, cy + r/2, cx + 2, cy, bg_col);
    } else if (!strcmp(name, "Time")) {
        d.drawCircle(cx, cy, r, col);
        d.drawLine(cx, cy, cx, cy - r + 2, col);
        d.drawLine(cx, cy, cx + r/2, cy, col);
    } else if (!strcmp(name, "Date")) {
        d.drawRoundRect(cx - r, cy - r, r * 2, r * 2, r/6, col);
        d.drawFastHLine(cx - r, cy - r/3, r * 2, col);
        d.fillRect(cx - r/2, cy - r + 2, 2, 4, col);
        d.fillRect(cx + r/2 - 2, cy - r + 2, 2, 4, col);
    } else if (!strcmp(name, "Uptime")) {
        d.fillTriangle(cx - r/2, cy - r, cx + r/2, cy - r, cx, cy, col);
        d.fillTriangle(cx - r/2, cy + r, cx + r/2, cy + r, cx, cy, col);
        d.drawFastHLine(cx - r/2, cy - r, r, col);
        d.drawFastHLine(cx - r/2, cy + r, r, col);
    } else if (!strcmp(name, "WiFi status")) {
        d.fillRect(cx - r/2, cy + r/2 - 2, 3, 3, col);
        d.fillRect(cx - r/6, cy + r/4 - 2, 3, r/2, col);
        d.fillRect(cx + r/6, cy - 2, 3, r - 2, col);
    } else if (!strcmp(name, "Bluetooth status")) {
        d.drawLine(cx, cy - r, cx, cy + r, col);
        d.drawLine(cx, cy - r, cx + r/2, cy - r/2, col);
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy + r/2, cx + r/2, cy + r/2, col);
        d.drawLine(cx + r/2, cy + r/2, cx - r/2, cy - r/2, col);
        d.drawLine(cx - r/2, cy - r/2, cx, cy - r, col);
    } else if (!strcmp(name, "Sound volume")) {
        d.fillRect(cx - r, cy - r/4, r/2, r/2, col);
        d.fillTriangle(cx - r/2, cy - r/4, cx - r/2, cy + r/4, cx, cy + r/2, col);
        d.fillTriangle(cx - r/2, cy - r/4, cx, cy - r/2, cx, cy + r/2, col);
        d.drawArc(cx + r/4, cy, r/3, r/3 + 2, 300, 60, col);
    } else if (!strcmp(name, "Steps")) {
        d.fillCircle(cx - r/4, cy - r/3, r/4, col);
        d.fillCircle(cx + r/4, cy + r/3, r/4, col);
    } else if (!strcmp(name, "Free RAM %")) {
        d.drawRect(cx - r/2, cy - r/2, r, r, col);
        for (int i = -1; i <= 1; i++) {
            d.drawLine(cx + i * r/3, cy - r/2, cx + i * r/3, cy - r, col);
            d.drawLine(cx + i * r/3, cy + r/2, cx + i * r/3, cy + r, col);
        }
    } else if (!strcmp(name, "Chip temp")) {
        d.drawRoundRect(cx - r/4, cy - r, r/2, r * 3 / 2, r/4, col);
        d.fillCircle(cx, cy + r/2, r/3, col);
    } else if (!strcmp(name, "Background color")) {
        d.fillRect(cx - r, cy - r/2, r, r, 0xFF0000);
        d.fillRect(cx, cy - r/2, r, r, 0x0000FF);
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
}

static void clamp_scroll(int visible_rows) {
    if (s_selected < s_scroll_top) s_scroll_top = s_selected;
    if (s_selected >= s_scroll_top + visible_rows) s_scroll_top = s_selected - visible_rows + 1;
    if (s_scroll_top < 0) s_scroll_top = 0;
    int max_top = K85_SB_ITEM_COUNT - visible_rows;
    if (max_top < 0) max_top = 0;
    if (s_scroll_top > max_top) s_scroll_top = max_top;
}

static void draw_plain(void) {
    clamp_scroll(K85_SB_VISIBLE_ROWS);

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(fg, bg);
    M5.Display.print("Status bar items");

    int y = 16;
    int last_visible = s_scroll_top + K85_SB_VISIBLE_ROWS;
    if (last_visible > K85_SB_ITEM_COUNT) last_visible = K85_SB_ITEM_COUNT;

    for (int i = s_scroll_top; i < last_visible; i++) {
        bool sel = (i == s_selected);
        M5.Display.setCursor(6, y);
        M5.Display.setTextColor(sel ? accent : fg, bg);
        M5.Display.print(sel ? "> " : "  ");
        M5.Display.print(k85_sb_items[i].label);

        char val[16];
        sb_value_str(val, sizeof(val), i);
        M5.Display.setCursor(170, y);
        M5.Display.print(val);
        y += 12;
    }

    if (K85_SB_ITEM_COUNT > K85_SB_VISIBLE_ROWS) {
        if (s_scroll_top > 0) {
            M5.Display.setCursor(225, 16);
            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.print("^");
        }
        if (last_visible < K85_SB_ITEM_COUNT) {
            M5.Display.setCursor(225, 16 + (K85_SB_VISIBLE_ROWS - 1) * 12);
            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.print("v");
        }
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(6, y + 4);
    M5.Display.print("A=next B=toggle A+B=back");
}

static void draw_list_icons(void) {
    const int visible_rows = 6;
    clamp_scroll(visible_rows);

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(accent, bg);
    M5.Display.print("Status bar items");

    int y = 16;
    const int line_h = 18;
    int last_visible = s_scroll_top + visible_rows;
    if (last_visible > K85_SB_ITEM_COUNT) last_visible = K85_SB_ITEM_COUNT;

    for (int i = s_scroll_top; i < last_visible; i++) {
        bool sel = (i == s_selected);
        if (sel) {
            M5.Display.fillRoundRect(2, y - 2, M5.Display.width() - 4, 15, 4, accent);
        }
        uint32_t item_fg = sel ? 0x000000 : fg;
        uint32_t item_bg = sel ? accent : bg;
        draw_sb_icon(10, y + 5, 6, k85_sb_items[i].label, item_fg, item_bg);

        M5.Display.setTextColor(item_fg, item_bg);
        M5.Display.setCursor(20, y + 1);
        M5.Display.print(k85_sb_items[i].label);

        char val[16];
        sb_value_str(val, sizeof(val), i);
        M5.Display.setCursor(170, y + 1);
        M5.Display.print(val);
        y += line_h;
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    if (s_scroll_top > 0) { M5.Display.setCursor(225, 16); M5.Display.print("^"); }
    if (last_visible < K85_SB_ITEM_COUNT) { M5.Display.setCursor(225, 16 + (visible_rows - 1) * line_h); M5.Display.print("v"); }
    M5.Display.setCursor(6, y + 4);
    M5.Display.print("A=next B=toggle A+B=back");
}

static void draw_grid(void) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int w = M5.Display.width();
    int h = M5.Display.height();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(accent, bg);
    M5.Display.print("Status bar items");

    const int cols = 4;
    const int rows = 2;
    const int per_page = cols * rows;
    const int start_y = 16;
    int grid_h = h - start_y - 10;
    int cell_w = w / cols;
    int cell_h = grid_h / rows;

    int page = s_selected / per_page;
    int page_count = (K85_SB_ITEM_COUNT + per_page - 1) / per_page;
    int page_start = page * per_page;
    int page_end = page_start + per_page;
    if (page_end > K85_SB_ITEM_COUNT) page_end = K85_SB_ITEM_COUNT;

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
        uint32_t item_fg = sel ? 0x000000 : fg;
        uint32_t item_bg = sel ? accent : bg;
        draw_sb_icon(cx, cy, 10, k85_sb_items[i].label, item_fg, item_bg);

        M5.Display.setTextSize(1);
        M5.Display.setTextColor(item_fg, item_bg);
        char short_label[16];
        int max_chars = (cell_w - 4) / 6;
        if (max_chars > 15) max_chars = 15;
        if (max_chars < 1) max_chars = 1;
        snprintf(short_label, sizeof(short_label), "%.*s", max_chars, k85_sb_items[i].label);
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
}

static void draw(void) {
    int style = g_config.menu_ui_style;
    switch (style) {
        case 1: draw_grid(); break;
        case 2: draw_list_icons(); break;
        default: draw_plain(); break;
    }
}

void k85_run_status_bar_settings(void) {
    s_selected = 0;
    s_scroll_top = 0;
    draw();

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            k85_config_save();
            return;
        }
        if (k85_btn_a_pressed()) {
            s_selected = (s_selected + 1) % K85_SB_ITEM_COUNT;
            draw();
        }
        if (k85_btn_b_pressed() && !k85_btn_a_is_down()) {
            uint32_t bit = k85_sb_items[s_selected].bit;
            if (bit != 0) {
                g_config.sb_flags ^= bit;
            } else {
                int cur_idx = 0;
                for (int p = 0; p < K85_SB_BG_PRESET_COUNT; p++) {
                    if (k85_sb_bg_presets[p] == g_config.sb_bg_color) { cur_idx = p; break; }
                }
                cur_idx = (cur_idx + 1) % K85_SB_BG_PRESET_COUNT;
                g_config.sb_bg_color = k85_sb_bg_presets[cur_idx];
            }
            k85_config_save();
            draw();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}