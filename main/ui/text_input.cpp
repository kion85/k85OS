#include "text_input.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>
#include <cctype>
#include "esp_timer.h"

static int64_t k85_ti_ticks_ms() { return esp_timer_get_time() / 1000; }

static const char *KB_ROW0[] = {"1","2","3","4","5","6","7","8","9","0"};
static const char *KB_ROW1_EN[] = {"Q","W","E","R","T","Y","U","I","O","P"};
static const char *KB_ROW2_EN[] = {"A","S","D","F","G","H","J","K","L"};
static const char *KB_ROW3_EN[] = {"Z","X","C","V","B","N","M","-","_","."};
static const char *KB_ROW1_RU[] = {"Й","Ц","У","К","Е","Н","Г","Ш","Щ","З","Х","Ъ"};
static const char *KB_ROW2_RU[] = {"Ф","Ы","В","А","П","Р","О","Л","Д","Ж","Э"};
static const char *KB_ROW3_RU[] = {"Я","Ч","С","М","И","Т","Ь","Б","Ю","Ё","."};
static const char *KB_ROW4_EN[] = {"SPACE","DEL","EXIT","OK","RU","CAPS"};
static const char *KB_ROW4_RU[] = {"SPACE","DEL","EXIT","OK","EN","CAPS"};

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

bool k85_text_input(const char *prompt, const char *initial, char *out, size_t out_size) {
    char text[128];
    snprintf(text, sizeof(text), "%s", initial ? initial : "");

    int st_row = 0, st_col = 0;
    int64_t last_a = 0, last_b = 0;
    int a_count = 0, b_count = 0;
    const int double_ms = 350;
    bool is_ru = false;
    bool is_caps = false;

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
            M5.Display.printf("...%s_", text + (len - 13));
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
        k85_draw_battery_icon();
    };

    draw();
    while (true) {
        k85_input_update();
        int64_t now = k85_ti_ticks_ms();
        KbLayout kb = is_ru ? kb_layout_ru() : kb_layout_en();

        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            if ((now - last_a) < double_ms && a_count == 1) {
                st_row = (st_row + 1) % 5;
                if (st_col > kb.counts[st_row] - 1) st_col = kb.counts[st_row] - 1;
                a_count = 0;
            } else {
                st_col = (st_col + 1) % kb.counts[st_row];
                a_count = 1;
            }
            last_a = now;
            draw();
        }
        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            const char *key = kb.rows[st_row][st_col];
            if (!strcmp(key, "SPACE")) {
                size_t l = strlen(text);
                if (l + 1 < sizeof(text)) { text[l] = ' '; text[l + 1] = 0; }
            } else if (!strcmp(key, "DEL")) {
                size_t l = strlen(text);
                if (l > 0) text[l - 1] = 0;
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
                    char c = key[0];
                    if (!is_caps && !is_ru) c = (char)tolower((unsigned char)c);
                    text[l] = c; text[l + 1] = 0;
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
    k85_draw_battery_icon();

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}


