#include "calculator.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

// Простой рекурсивно-нисходящий парсер +-*/() вместо Python eval() (в C++ eval() нет —
// логика та же: разрешены только цифры/точка/+-*/()/пробел, поведение при ошибке
// идентично оригиналу — вывод "Error").
static const char *g_calc_ptr;
static bool g_calc_error;

static double calc_parse_expr(void);

static double calc_parse_factor(void) {
    while (*g_calc_ptr == ' ') g_calc_ptr++;
    if (*g_calc_ptr == '(') {
        g_calc_ptr++;
        double v = calc_parse_expr();
        while (*g_calc_ptr == ' ') g_calc_ptr++;
        if (*g_calc_ptr == ')') g_calc_ptr++; else g_calc_error = true;
        return v;
    }
    if (*g_calc_ptr == '-') { g_calc_ptr++; return -calc_parse_factor(); }
    if (*g_calc_ptr == '+') { g_calc_ptr++; return calc_parse_factor(); }
    char *end = nullptr;
    double v = strtod(g_calc_ptr, &end);
    if (end == g_calc_ptr) { g_calc_error = true; return 0; }
    g_calc_ptr = end;
    return v;
}

static double calc_parse_term(void) {
    double v = calc_parse_factor();
    while (true) {
        while (*g_calc_ptr == ' ') g_calc_ptr++;
        if (*g_calc_ptr == '*') { g_calc_ptr++; v *= calc_parse_factor(); }
        else if (*g_calc_ptr == '/') {
            g_calc_ptr++;
            double d = calc_parse_factor();
            if (d == 0) { g_calc_error = true; return 0; }
            v /= d;
        } else break;
    }
    return v;
}

static double calc_parse_expr(void) {
    double v = calc_parse_term();
    while (true) {
        while (*g_calc_ptr == ' ') g_calc_ptr++;
        if (*g_calc_ptr == '+') { g_calc_ptr++; v += calc_parse_term(); }
        else if (*g_calc_ptr == '-') { g_calc_ptr++; v -= calc_parse_term(); }
        else break;
    }
    return v;
}

static void calc_eval(const char *s, char *out, size_t out_size) {
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!(isdigit((unsigned char)c) || c == '.' || c == '+' || c == '-' ||
              c == '*' || c == '/' || c == '(' || c == ')' || c == ' ')) {
            snprintf(out, out_size, "Error");
            return;
        }
    }
    g_calc_ptr = s;
    g_calc_error = false;
    double r = calc_parse_expr();
    while (*g_calc_ptr == ' ') g_calc_ptr++;
    if (g_calc_error || *g_calc_ptr != 0) {
        snprintf(out, out_size, "Error");
        return;
    }
    long long ri = (long long)r;
    if ((double)ri == r) {
        snprintf(out, out_size, "%lld", ri);
    } else {
        snprintf(out, out_size, "%.4f", r);
    }
}

static const char *const CALC_ROWS[5][4] = {
    {"7", "8", "9", "/"},
    {"4", "5", "6", "*"},
    {"1", "2", "3", "-"},
    {"C", "0", ".", "+"},
    {"DEL", "=", nullptr, nullptr},
};
static const int CALC_ROW_LEN[5] = {4, 4, 4, 4, 2};

void k85_run_calculator(void) {
    char expr[64] = "";
    char result[32] = "";
    int sel_x = 0, sel_y = 0;

    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    auto draw = [&]() {
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(2);
        M5.Display.fillRect(4, 4, W - 8, 28, 0x111111);
        M5.Display.setTextColor(0xFFFFFF, 0x111111);
        int len = (int)strlen(expr);
        M5.Display.setCursor(6, 8);
        if (len <= 14) {
            M5.Display.printf("%s_", expr);
        } else {
            M5.Display.printf("...%s_", expr + (len - 11));
        }
        if (result[0]) {
            M5.Display.fillRect(4, 34, W - 8, 20, 0x001122);
            M5.Display.setTextColor(0x00FFFF, 0x001122);
            M5.Display.setCursor(6, 36);
            M5.Display.printf("= %s", result);
        }

        int btn_w = (W - 8) / 4;
        int btn_h = (H - 62) / 5; if (btn_h > 20) btn_h = 20;
        int off_x = 4, off_y = 56;
        for (int y = 0; y < 5; y++) {
            for (int x = 0; x < CALC_ROW_LEN[y]; x++) {
                const char *label = CALC_ROWS[y][x];
                int bx = off_x + x * btn_w;
                int by = off_y + y * btn_h;
                bool sel = (x == sel_x && y == sel_y);
                uint32_t bgc = sel ? accent : 0x222222;
                uint32_t fgc = sel ? 0x000000 : fg;
                M5.Display.fillRect(bx + 1, by + 1, btn_w - 2, btn_h - 2, bgc);
                M5.Display.setTextSize(1);
                M5.Display.setTextColor(fgc, bgc);
                int lbl_len = (int)strlen(label);
                int tx = bx + (btn_w - lbl_len * 6) / 2;
                int ty = by + (btn_h - 8) / 2;
                M5.Display.setCursor(tx, ty);
                M5.Display.print(label);
            }
        }
        k85_draw_battery_icon();
    };

    draw();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            sel_x++;
            if (sel_x >= CALC_ROW_LEN[sel_y]) {
                sel_x = 0;
                sel_y = (sel_y + 1) % 5;
                if (sel_x >= CALC_ROW_LEN[sel_y]) sel_x = CALC_ROW_LEN[sel_y] - 1;
            }
            draw();
        }
        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            const char *label = CALC_ROWS[sel_y][sel_x];
            size_t elen = strlen(expr);
            if (!strcmp(label, "C")) {
                expr[0] = 0;
                result[0] = 0;
            } else if (!strcmp(label, "DEL")) {
                if (elen > 0) expr[elen - 1] = 0;
                result[0] = 0;
            } else if (!strcmp(label, "=")) {
                if (expr[0]) calc_eval(expr, result, sizeof(result));
                else result[0] = 0;
            } else {
                if (elen + 1 < sizeof(expr)) {
                    expr[elen] = label[0];
                    expr[elen + 1] = 0;
                }
                if (strchr("+-*/", label[0])) {
                    // live-preview промежуточного результата, как в оригинале
                    char partial[64];
                    snprintf(partial, sizeof(partial), "%s", expr);
                    size_t pl = strlen(partial);
                    if (pl > 0) partial[pl - 1] = 0;
                    if (partial[0]) calc_eval(partial, result, sizeof(result));
                }
            }
            draw();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
