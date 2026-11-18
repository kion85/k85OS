#include "text_input.h"
#include "esp_attr.h"
#include "theme.h"
#include "battery.h"
#include "status_bar.h"
#include "power.h"
#include "input.h"
#include "config.h"
#include "config.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>
#include <cctype>
#include <cmath>
#include "esp_timer.h"

static int64_t k85_ti_ticks_ms() { return esp_timer_get_time() / 1000; }
// K85RU1: Russian caps -> lower case on a 2-byte UTF-8 letter, UTF-8 safe tail of a string
static void k85_ru_lower(char *b) {
    unsigned char a = (unsigned char)b[0], c = (unsigned char)b[1];
    if (a == 0xD0 && c >= 0x90 && c <= 0x9F) b[1] = (char)(c + 0x20);
    else if (a == 0xD0 && c >= 0xA0 && c <= 0xAF) { b[0] = (char)0xD1; b[1] = (char)(c - 0x20); }
    else if (a == 0xD0 && c == 0x81) { b[0] = (char)0xD1; b[1] = (char)0x91; }
}
static const char *k85_utf8_tail(const char *s, int off) { while (s[off] && (((unsigned char)s[off]) & 0xC0) == 0x80) off++; return s + off; }

static const char *KB_ROW0[] = {"1","2","3","4","5","6","7","8","9","0"};
static const char *KB_ROW1_EN[] = {"Q","W","E","R","T","Y","U","I","O","P"};
static const char *KB_ROW2_EN[] = {"A","S","D","F","G","H","J","K","L"};
static const char *KB_ROW3_EN[] = {"Z","X","C","V","B","N","M","-","_","."};
static const char *KB_ROW1_RU[] = {"Й","Ц","У","К","Е","Н","Г","Ш","Щ","З","Х","Ъ"};
static const char *KB_ROW2_RU[] = {"Ф","Ы","В","А","П","Р","О","Л","Д","Ж","Э"};
static const char *KB_ROW3_RU[] = {"Я","Ч","С","М","И","Т","Ь","Б","Ю","Ё","."};
static const char *KB_ROW4_EN[] = {"SPACE","DEL","EXIT","OK","RU","CAPS"};
static const char *KB_ROW4_RU[] = {"SPACE","DEL","EXIT","OK","EN","CAPS"};

// Отдельные раскладки нижнего ряда для многострочного редактора - добавлена
// клавиша ENTER. Держим отдельно от однострочных KB_ROW4_*, чтобы не
// затронуть k85_text_input() (там ENTER была бы бессмысленной/опасной клавишей).
static const char *KB_ROW4_EN_ML[] = {"SPACE","ENTER","DEL","EXIT","OK","RU","CAPS"};
static const char *KB_ROW4_RU_ML[] = {"SPACE","ENTER","DEL","EXIT","OK","EN","CAPS"};

struct KbLayout {
    const char *const *rows[5];
    int counts[5];
};

static KbLayout kb_layout_en() {
    return { { KB_ROW0, KB_ROW1_EN, KB_ROW2_EN, KB_ROW3_EN, KB_ROW4_EN },
             { 10, 10, 9, 10, 6 } };
}
static KbLayout kb_layout_ru() {
    return { { KB_ROW0, KB_ROW1_RU, KB_ROW2_RU, KB_ROW3_RU, KB_ROW4_RU },
             { 10, 12, 11, 11, 6 } };
}
static KbLayout kb_layout_en_ml() {
    return { { KB_ROW0, KB_ROW1_EN, KB_ROW2_EN, KB_ROW3_EN, KB_ROW4_EN_ML },
             { 10, 10, 9, 10, 7 } };
}
static KbLayout kb_layout_ru_ml() {
    return { { KB_ROW0, KB_ROW1_RU, KB_ROW2_RU, KB_ROW3_RU, KB_ROW4_RU_ML },
             { 10, 12, 11, 11, 7 } };
}

bool k85_text_input(const char *prompt, const char *initial, char *out, size_t out_size) {
    char text[128];
    snprintf(text, sizeof(text), "%s", initial ? initial : "");

    int st_row = 0, st_col = 0;
    int64_t a_press_start = 0;
    bool a_was_down = false;
    bool a_hold_triggered = false;
    const int hold_ms = 400;
    int64_t last_imu_step_ms = 0;
    const int64_t imu_step_cooldown_ms = 220;
    const float imu_threshold = 0.35f;
    bool is_ru = false;
    bool is_caps = false;
    int64_t last_activity_ms = k85_ti_ticks_ms();
    bool kb_hidden = false;
    #define K85_TI_KB_IDLE_MS 5000

    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    auto draw = [&]() {
        KbLayout kb = is_ru ? kb_layout_ru() : kb_layout_en();
        if (st_col >= kb.counts[st_row]) st_col = kb.counts[st_row] - 1;

        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.printf("%s [%s/%s]", prompt, is_ru ? "RU" : "EN", is_caps ? "CAPS" : "low");

        M5.Display.setTextSize(2);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 16);
        int len = (int)strlen(text);
        if (len <= 16) {
            M5.Display.printf("%s_", text);
        } else {
            M5.Display.printf("...%s_", k85_utf8_tail(text, len - 13));
        }

        if (kb_hidden) {
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(0x666666, bg);
            const char *hint = "(press any button)";
            int hx = (W - (int)strlen(hint) * 6) / 2;
            M5.Display.setCursor(hx, H / 2);
            M5.Display.print(hint);
            k85_status_bar_overlay();
            return;
        }

        int top = 38;
        int row_h = (H - top - 2) / 5;
        if (row_h < 12) row_h = 12;

        for (int r = 0; r < 5; r++) {
            int n = kb.counts[r];
            int col_w = W / n;
            for (int c = 0; c < n; c++) {
                int x = c * col_w;
                int y = top + r * row_h;
                bool sel = (r == st_row && c == st_col);
                uint32_t bgc = sel ? accent : 0x222222;
                uint32_t fgc = sel ? 0x000000 : fg;
                M5.Display.fillRect(x + 1, y + 1, col_w - 2, row_h - 2, bgc);
                const char *label = kb.rows[r][c];
                M5.Display.setTextSize(1);
                M5.Display.setTextColor(fgc, bgc);
                int lbl_len = (int)strlen(label);
                int tx = x + (col_w - lbl_len * 6) / 2; if (tx < x) tx = x;
                int ty = y + (row_h - 8) / 2; if (ty < y) ty = y;
                M5.Display.setCursor(tx, ty);
                M5.Display.print(label);
            }
        }
        k85_status_bar_overlay();
    };

    draw();
    while (true) {
        k85_input_update();
        int64_t now = k85_ti_ticks_ms();
        KbLayout kb = is_ru ? kb_layout_ru() : kb_layout_en();

        float imu_ax = 0, imu_ay = 0, imu_az = 0;
        if (g_config.kbd_nav_mode == 1) {
            M5.Imu.getAccel(&imu_ax, &imu_ay, &imu_az); // TODO: сверь с air_mouse.cpp, если сигнатура другая
        }
        bool imu_tilted = (g_config.kbd_nav_mode == 1) &&
                           (fabsf(imu_ax) > imu_threshold || fabsf(imu_ay) > imu_threshold);
        bool any_input = k85_btn_a_is_down() || k85_btn_b_is_down() || imu_tilted;
        if (any_input) {
            if (kb_hidden) {
                kb_hidden = false;
                last_activity_ms = now;
                draw();
                vTaskDelay(pdMS_TO_TICKS(30));
                continue; // это нажатие только "будит" клавиатуру, не вводит символ
            }
            last_activity_ms = now;
        } else if (!kb_hidden && (now - last_activity_ms) >= K85_TI_KB_IDLE_MS) {
            kb_hidden = true;
            draw();
        }

        if (g_config.kbd_nav_mode == 1) {
            if (now - last_imu_step_ms >= imu_step_cooldown_ms) {
                bool moved = false;
                if (imu_ax > imu_threshold) {
                    st_col = (st_col + 1) % kb.counts[st_row];
                    moved = true;
                } else if (imu_ax < -imu_threshold) {
                    st_col = (st_col - 1 + kb.counts[st_row]) % kb.counts[st_row];
                    moved = true;
                } else if (imu_ay > imu_threshold) {
                    st_row = (st_row + 1) % 5;
                    if (st_col > kb.counts[st_row] - 1) st_col = kb.counts[st_row] - 1;
                    moved = true;
                } else if (imu_ay < -imu_threshold) {
                    st_row = (st_row - 1 + 5) % 5;
                    if (st_col > kb.counts[st_row] - 1) st_col = kb.counts[st_row] - 1;
                    moved = true;
                }
                if (moved) {
                    k85_wake_screen();
                    last_imu_step_ms = now;
                    draw();
                }
            }
        } else {
            bool a_down_now = k85_btn_a_is_down();
            if (a_down_now && !a_was_down) {
                a_press_start = now;
                a_hold_triggered = false;
            }
            if (a_down_now && !a_hold_triggered && (now - a_press_start) >= hold_ms) {
                st_row = (st_row + 1) % 5;
                if (st_col > kb.counts[st_row] - 1) st_col = kb.counts[st_row] - 1;
                a_hold_triggered = true;
                draw();
            }
            if (!a_down_now && a_was_down && !a_hold_triggered) {
                k85_wake_screen();
                st_col = (st_col + 1) % kb.counts[st_row];
                draw();
            }
            a_was_down = a_down_now;
        }
        bool k85_tap = false; { int tx_, ty_; if (k85_pointer_take(&tx_, &ty_)) { if (kb_hidden) { kb_hidden = false; last_activity_ms = k85_ti_ticks_ms(); draw(); } else { int top_ = 38; int rh_ = (H - top_ - 2) / 5; if (rh_ < 12) rh_ = 12; int r_ = (ty_ - top_) / rh_; if (ty_ >= top_ && r_ >= 0 && r_ < 5) { int n_ = kb.counts[r_]; int c_ = tx_ / (W / n_); if (c_ >= 0 && c_ < n_) { st_row = r_; st_col = c_; k85_tap = true; } } } } } if (k85_btn_b_pressed() || k85_tap) {
            k85_wake_screen();
            const char *key = kb.rows[st_row][st_col];
            if (!strcmp(key, "SPACE")) {
                size_t l = strlen(text);
                if (l + 1 < sizeof(text)) { text[l] = ' '; text[l + 1] = 0; }
            } else if (!strcmp(key, "DEL")) {
                size_t l = strlen(text);
                if (l > 0) { l--; while (l > 0 && (((unsigned char)text[l]) & 0xC0) == 0x80) l--; text[l] = 0; }
            } else if (!strcmp(key, "EXIT")) {
                return false;
            } else if (!strcmp(key, "OK")) {
                snprintf(out, out_size, "%s", text);
                return true;
            } else if (!strcmp(key, "EN") || !strcmp(key, "RU")) {
                is_ru = !strcmp(key, "RU");
                st_row = 0; st_col = 0;
            } else if (!strcmp(key, "CAPS")) {
                is_caps = !is_caps;
            } else {
                size_t l = strlen(text);
                if (l + 1 < sizeof(text)) {
                    if (is_ru && (unsigned char)key[0] >= 0xD0 && key[1]) { char b2[3] = { key[0], key[1], 0 }; if (!is_caps) k85_ru_lower(b2); if (l + 2 < sizeof(text)) { text[l] = b2[0]; text[l + 1] = b2[1]; text[l + 2] = 0; } } else { char c = key[0]; if (!is_caps && !is_ru) c = (char)tolower((unsigned char)c); text[l] = c; text[l + 1] = 0; }
                }
            }
            draw();
        }
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            snprintf(out, out_size, "%s", text);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

bool k85_text_input_multiline(const char *prompt, const char *initial, char *out, size_t out_size) {
    static EXT_RAM_BSS_ATTR char text[2048];
    snprintf(text, sizeof(text), "%s", initial ? initial : "");

    int st_row = 0, st_col = 0;
    int64_t a_press_start = 0;
    bool a_was_down = false;
    bool a_hold_triggered = false;
    const int hold_ms = 400;
    int64_t last_imu_step_ms = 0;
    const int64_t imu_step_cooldown_ms = 220;
    const float imu_threshold = 0.35f;
    bool is_ru = false;
    bool is_caps = false;
    int64_t last_activity_ms = k85_ti_ticks_ms();
    bool kb_hidden = false;

    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    const int preview_rows = 3;
    const int chars_per_row = W / 6;

    auto draw = [&]() {
        KbLayout kb = is_ru ? kb_layout_ru_ml() : kb_layout_en_ml();
        if (st_col >= kb.counts[st_row]) st_col = kb.counts[st_row] - 1;

        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.printf("%s [%s/%s]", prompt, is_ru ? "RU" : "EN", is_caps ? "CAPS" : "low");

        // Показываем "хвост" буфера - последние ~preview_rows*chars_per_row
        // символов. При наборе длинного текста видимая часть естественно
        // сдвигается вниз, показывая самое свежее (как обычный многострочный
        // ввод, без отдельной навигации курсором).
        size_t total_len = strlen(text);
        size_t tail_chars = (size_t)(preview_rows * chars_per_row);
        size_t start = total_len > tail_chars ? total_len - tail_chars : 0;

        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 14);
        M5.Display.setTextWrap(true, false);
        M5.Display.printf("%s_", k85_utf8_tail(text, (int)start));
        M5.Display.setTextWrap(false, false);

        if (kb_hidden) {
            M5.Display.setTextColor(0x666666, bg);
            const char *hint = "(press any button)";
            int hx = (W - (int)strlen(hint) * 6) / 2;
            M5.Display.setCursor(hx, H / 2);
            M5.Display.print(hint);
            k85_status_bar_overlay();
            return;
        }

        int top = 14 + preview_rows * 10 + 4;
        int row_h = (H - top - 2) / 5;
        if (row_h < 12) row_h = 12;

        for (int r = 0; r < 5; r++) {
            int n = kb.counts[r];
            int col_w = W / n;
            for (int c = 0; c < n; c++) {
                int x = c * col_w;
                int y = top + r * row_h;
                bool sel = (r == st_row && c == st_col);
                uint32_t bgc = sel ? accent : 0x222222;
                uint32_t fgc = sel ? 0x000000 : fg;
                M5.Display.fillRect(x + 1, y + 1, col_w - 2, row_h - 2, bgc);
                const char *label = kb.rows[r][c];
                M5.Display.setTextSize(1);
                M5.Display.setTextColor(fgc, bgc);
                int lbl_len = (int)strlen(label);
                int tx = x + (col_w - lbl_len * 6) / 2; if (tx < x) tx = x;
                int ty = y + (row_h - 8) / 2; if (ty < y) ty = y;
                M5.Display.setCursor(tx, ty);
                M5.Display.print(label);
            }
        }
        k85_status_bar_overlay();
    };

    draw();
    while (true) {
        k85_input_update();
        int64_t now = k85_ti_ticks_ms();
        KbLayout kb = is_ru ? kb_layout_ru_ml() : kb_layout_en_ml();

        float imu_ax = 0, imu_ay = 0, imu_az = 0;
        if (g_config.kbd_nav_mode == 1) {
            M5.Imu.getAccel(&imu_ax, &imu_ay, &imu_az); // TODO: сверь с air_mouse.cpp, если сигнатура другая
        }
        bool imu_tilted = (g_config.kbd_nav_mode == 1) &&
                           (fabsf(imu_ax) > imu_threshold || fabsf(imu_ay) > imu_threshold);
        bool any_input = k85_btn_a_is_down() || k85_btn_b_is_down() || imu_tilted;
        if (any_input) {
            if (kb_hidden) {
                kb_hidden = false;
                last_activity_ms = now;
                draw();
                vTaskDelay(pdMS_TO_TICKS(30));
                continue;
            }
            last_activity_ms = now;
        } else if (!kb_hidden && (now - last_activity_ms) >= K85_TI_KB_IDLE_MS) {
            kb_hidden = true;
            draw();
        }

        if (g_config.kbd_nav_mode == 1) {
            if (now - last_imu_step_ms >= imu_step_cooldown_ms) {
                bool moved = false;
                if (imu_ax > imu_threshold) {
                    st_col = (st_col + 1) % kb.counts[st_row];
                    moved = true;
                } else if (imu_ax < -imu_threshold) {
                    st_col = (st_col - 1 + kb.counts[st_row]) % kb.counts[st_row];
                    moved = true;
                } else if (imu_ay > imu_threshold) {
                    st_row = (st_row + 1) % 5;
                    if (st_col > kb.counts[st_row] - 1) st_col = kb.counts[st_row] - 1;
                    moved = true;
                } else if (imu_ay < -imu_threshold) {
                    st_row = (st_row - 1 + 5) % 5;
                    if (st_col > kb.counts[st_row] - 1) st_col = kb.counts[st_row] - 1;
                    moved = true;
                }
                if (moved) {
                    k85_wake_screen();
                    last_imu_step_ms = now;
                    draw();
                }
            }
        } else {
            bool a_down_now = k85_btn_a_is_down();
            if (a_down_now && !a_was_down) {
                a_press_start = now;
                a_hold_triggered = false;
            }
            if (a_down_now && !a_hold_triggered && (now - a_press_start) >= hold_ms) {
                st_row = (st_row + 1) % 5;
                if (st_col > kb.counts[st_row] - 1) st_col = kb.counts[st_row] - 1;
                a_hold_triggered = true;
                draw();
            }
            if (!a_down_now && a_was_down && !a_hold_triggered) {
                k85_wake_screen();
                st_col = (st_col + 1) % kb.counts[st_row];
                draw();
            }
            a_was_down = a_down_now;
        }
        bool k85_tap = false; { int tx_, ty_; if (k85_pointer_take(&tx_, &ty_)) { if (kb_hidden) { kb_hidden = false; last_activity_ms = k85_ti_ticks_ms(); draw(); } else { int top_ = 48; int rh_ = (H - top_ - 2) / 5; if (rh_ < 12) rh_ = 12; int r_ = (ty_ - top_) / rh_; if (ty_ >= top_ && r_ >= 0 && r_ < 5) { int n_ = kb.counts[r_]; int c_ = tx_ / (W / n_); if (c_ >= 0 && c_ < n_) { st_row = r_; st_col = c_; k85_tap = true; } } } } } if (k85_btn_b_pressed() || k85_tap) {
            k85_wake_screen();
            const char *key = kb.rows[st_row][st_col];
            if (!strcmp(key, "SPACE")) {
                size_t l = strlen(text);
                if (l + 1 < sizeof(text)) { text[l] = ' '; text[l + 1] = 0; }
            } else if (!strcmp(key, "ENTER")) {
                size_t l = strlen(text);
                if (l + 1 < sizeof(text)) { text[l] = '\n'; text[l + 1] = 0; }
            } else if (!strcmp(key, "DEL")) {
                size_t l = strlen(text);
                if (l > 0) { l--; while (l > 0 && (((unsigned char)text[l]) & 0xC0) == 0x80) l--; text[l] = 0; }
            } else if (!strcmp(key, "EXIT")) {
                return false;
            } else if (!strcmp(key, "OK")) {
                snprintf(out, out_size, "%s", text);
                return true;
            } else if (!strcmp(key, "EN") || !strcmp(key, "RU")) {
                is_ru = !strcmp(key, "RU");
                st_row = 0; st_col = 0;
            } else if (!strcmp(key, "CAPS")) {
                is_caps = !is_caps;
            } else {
                size_t l = strlen(text);
                if (l + 1 < sizeof(text)) {
                    if (is_ru && (unsigned char)key[0] >= 0xD0 && key[1]) { char b2[3] = { key[0], key[1], 0 }; if (!is_caps) k85_ru_lower(b2); if (l + 2 < sizeof(text)) { text[l] = b2[0]; text[l + 1] = b2[1]; text[l + 2] = 0; } } else { char c = key[0]; if (!is_caps && !is_ru) c = (char)tolower((unsigned char)c); text[l] = c; text[l + 1] = 0; }
                }
            }
            draw();
        }
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            snprintf(out, out_size, "%s", text);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_area_show(const char *const lines[], int count, const char *title) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(accent, bg);
    M5.Display.setCursor(4, 2);
    M5.Display.print(title);

    int y = 16;
    for (int i = 0; i < count; i++) {
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, y);
        M5.Display.print(lines[i]);
        y += 12;
    }
    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(4, M5.Display.height() - 12);
    M5.Display.print("A+B=back");
    k85_status_bar_overlay();

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
