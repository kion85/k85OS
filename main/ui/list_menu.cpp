#include "list_menu.h"
#include "theme.h"
#include "log.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "config.h"
#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>
#include <cctype>

// Универсальная иконка-фолбэк для пунктов, для которых вызывающий код не
// передал свой icon_fn: кружок акцентного цвета с первой заглавной буквой
// названия внутри. Работает для любого текста без ручной прорисовки.
static void draw_generic_icon(int cx, int cy, int r, const char *label, uint32_t col, uint32_t bg_col) {
    M5.Display.fillCircle(cx, cy, r, col);
    char ch = label && label[0] ? (char)toupper((unsigned char)label[0]) : '?';
    char buf[2] = { ch, 0 };
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(bg_col, col);
    M5.Display.setCursor(cx - 3, cy - 4);
    M5.Display.print(buf);
}

static inline void draw_item_icon(K85IconDrawFn icon_fn, int cx, int cy, int r,
                                   const char *label, uint32_t col, uint32_t bg_col) {
    if (icon_fn) icon_fn(cx, cy, r, label, col, bg_col);
    else draw_generic_icon(cx, cy, r, label, col, bg_col);
}

static void format_label(char *out, size_t out_size, const char *item,
                          const char *const score_keys[], int i) {
    if (score_keys && score_keys[i]) {
        snprintf(out, out_size, "%s (%d)", item, k85_get_high_score(score_keys[i]));
    } else {
        snprintf(out, out_size, "%s", item);
    }
}

static void draw_list_plain(const char *title, const char *const items[], int count,
                             const char *const score_keys[], int sel, int scroll,
                             uint32_t bg, uint32_t fg, uint32_t accent) {
    int H = M5.Display.height();
    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(accent, bg);
    M5.Display.setCursor(4, 2);
    M5.Display.print(title);
    M5.Display.setTextSize(2);
    const int line_h = 24;
    const int start_y = 16;
    int visible = (H - start_y) / line_h;
    if (visible < 1) visible = 1;
    int end = count < (scroll + visible) ? count : (scroll + visible);
    for (int i = scroll; i < end; i++) {
        char label[64];
        format_label(label, sizeof(label), items[i], score_keys, i);
        int yy = start_y + (i - scroll) * line_h;
        if (i == sel) {
            M5.Display.setTextColor(0x000000, accent);
            M5.Display.setCursor(4, yy);
            M5.Display.printf(">%s", label);
        } else {
            M5.Display.setTextColor(fg, bg);
            M5.Display.setCursor(4, yy);
            M5.Display.printf(" %s", label);
        }
    }
    k85_draw_battery_icon();
}

static void draw_list_icons(const char *title, const char *const items[], int count,
                             const char *const score_keys[], int sel, int scroll,
                             uint32_t bg, uint32_t fg, uint32_t accent, K85IconDrawFn icon_fn) {
    int H = M5.Display.height();
    int W = M5.Display.width();
    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(accent, bg);
    M5.Display.setCursor(4, 2);
    M5.Display.print(title);

    const int line_h = 22;
    const int start_y = 16;
    int visible = (H - start_y) / line_h;
    if (visible < 1) visible = 1;
    int end = count < (scroll + visible) ? count : (scroll + visible);

    M5.Display.setTextSize(1);
    for (int i = scroll; i < end; i++) {
        char label[64];
        format_label(label, sizeof(label), items[i], score_keys, i);
        int yy = start_y + (i - scroll) * line_h;
        bool is_sel = (i == sel);
        if (is_sel) {
            M5.Display.fillRoundRect(2, yy - 2, W - 4, 16, 4, accent);
        }
        uint32_t item_fg = is_sel ? 0x000000 : fg;
        uint32_t item_bg = is_sel ? accent : bg;
        draw_item_icon(icon_fn, 14, yy + 6, 8, items[i], item_fg, item_bg);
        M5.Display.setTextColor(item_fg, item_bg);
        M5.Display.setCursor(26, yy + 2);
        M5.Display.print(label);
    }
    k85_draw_battery_icon();
}

static void draw_list_grid(const char *title, const char *const items[], int count,
                            const char *const score_keys[], int sel,
                            uint32_t bg, uint32_t fg, uint32_t accent, K85IconDrawFn icon_fn) {
    int H = M5.Display.height();
    int W = M5.Display.width();
    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(accent, bg);
    M5.Display.setCursor(4, 2);
    M5.Display.print(title);

    const int cols = 4;
    const int rows = 2;
    const int per_page = cols * rows;
    const int start_y = 16;
    int grid_h = H - start_y - 10;
    int cell_w = W / cols;
    int cell_h = grid_h / rows;

    int page = sel / per_page;
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

        bool is_sel = (i == sel);
        if (is_sel) {
            M5.Display.fillRoundRect(col * cell_w + 3, start_y + row * cell_h + 2,
                                      cell_w - 6, cell_h - 4, 6, accent);
        }
        uint32_t item_fg = is_sel ? 0x000000 : fg;
        uint32_t item_bg = is_sel ? accent : bg;
        draw_item_icon(icon_fn, cx, cy, 12, items[i], item_fg, item_bg);

        char label[64];
        format_label(label, sizeof(label), items[i], score_keys, i);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(item_fg, item_bg);
        char short_label[16];
        int max_chars = (cell_w - 4) / 6;
        if (max_chars > 15) max_chars = 15;
        if (max_chars < 1) max_chars = 1;
        snprintf(short_label, sizeof(short_label), "%.*s", max_chars, label);
        int tx = col * cell_w + (cell_w - (int)strlen(short_label) * 6) / 2;
        if (tx < col * cell_w) tx = col * cell_w + 1;
        M5.Display.setCursor(tx, start_y + row * cell_h + cell_h - 12);
        M5.Display.print(short_label);
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    if (page_count > 1) {
        char pg[32];
        snprintf(pg, sizeof(pg), "%d/%d", page + 1, page_count);
        M5.Display.setCursor(W - (int)strlen(pg) * 6 - 4, H - 10);
        M5.Display.print(pg);
    }
    k85_draw_battery_icon();
}

int k85_run_list_menu(const char *title, const char *const items[], int count,
                       const char *const score_keys[], K85IconDrawFn icon_fn) {
    k85_log("list_menu ENTER: title=%s count=%d btnB_down=%d", title, count, (int)k85_btn_b_is_down());
    if (count <= 0) return -1;
    int sel = 0;
    int scroll = 0;
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int H = M5.Display.height();
    int style = g_config.menu_ui_style; // 0=list, 1=grid+icons, 2=list+icons (та же схема, что и в главном меню)

    while (true) {
        if (sel >= count) sel = count - 1;

        int line_h = (style == 2) ? 22 : 24;
        int start_y = 16;
        int visible = (H - start_y) / line_h;
        if (visible < 1) visible = 1;
        if (sel < scroll) scroll = sel;
        else if (sel >= scroll + visible) scroll = sel - visible + 1;

        switch (style) {
            case 1:
                draw_list_grid(title, items, count, score_keys, sel, bg, fg, accent, icon_fn);
                break;
            case 2:
                draw_list_icons(title, items, count, score_keys, sel, scroll, bg, fg, accent, icon_fn);
                break;
            default:
                draw_list_plain(title, items, count, score_keys, sel, scroll, bg, fg, accent);
                break;
        }

        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) {
                k85_wait_ab_release();
                return -1;
            }
            if (k85_btn_a_pressed()) {
                k85_wake_screen();
                sel = (sel + 1) % count;
                break;
            }
            if (k85_btn_b_pressed()) {
                k85_wake_screen();
                k85_log("list_menu B pressed: title=%s sel=%d item=%s", title, sel, items[sel]);
                if (!strcmp(items[sel], "Back")) return -1;
                return sel;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}