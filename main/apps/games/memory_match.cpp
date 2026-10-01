#include "memory_match.h"
#include "theme.h"
#include "battery.h"
#include "input.h"
#include "common.h"
#include "config.h"
#include "sound.h"
#include "power.h"

#include "M5Unified.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

#define K85_MEM_COLS 4
#define K85_MEM_ROWS 4
#define K85_MEM_CARDS (K85_MEM_COLS * K85_MEM_ROWS)
#define K85_MEM_PAIRS (K85_MEM_CARDS / 2)

static const uint32_t K85_MEM_COLORS[K85_MEM_PAIRS] = {
    0xFF5555, 0x55FF55, 0x5599FF, 0xFFFF55, 0xFF55FF, 0x55FFFF, 0xFFAA33, 0xBBBBBB
};

static void draw_memory(const uint8_t *vals, const bool *revealed, const bool *matched,
                        int sel, int moves) {
    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(accent, bg);
    M5.Display.setCursor(4, 3);
    M5.Display.printf("Memory  moves:%d", moves);

    const int top = 14;
    const int bottom_pad = 12;
    int cell_w = W / K85_MEM_COLS;
    int cell_h = (H - top - bottom_pad) / K85_MEM_ROWS;

    for (int i = 0; i < K85_MEM_CARDS; i++) {
        int col = i % K85_MEM_COLS;
        int row = i / K85_MEM_COLS;
        int x = col * cell_w;
        int y = top + row * cell_h;
        int rx = x + 2, ry = y + 2, rw = cell_w - 4, rh = cell_h - 4;

        uint32_t box_col;
        uint32_t txt_col;
        char label[2] = { '?', 0 };
        if (matched[i]) {
            box_col = 0x1E2A1E;
            txt_col = K85_MEM_COLORS[vals[i]];
            label[0] = (char)('A' + vals[i]);
        } else if (revealed[i]) {
            box_col = K85_MEM_COLORS[vals[i]];
            txt_col = 0x000000;
            label[0] = (char)('A' + vals[i]);
        } else {
            box_col = 0x333355;
            txt_col = 0xAAAAAA;
        }
        M5.Display.fillRoundRect(rx, ry, rw, rh, 3, box_col);

        int s = (rh >= 18 && rw >= 14) ? 2 : 1;
        int cw = 6 * s, ch = 8 * s;
        M5.Display.setTextSize(s);
        M5.Display.setTextColor(txt_col, box_col);
        M5.Display.setCursor(rx + (rw - cw) / 2, ry + (rh - ch) / 2);
        M5.Display.print(label);

        if (i == sel) {
            M5.Display.drawRoundRect(x + 1, y + 1, cell_w - 2, cell_h - 2, 4, 0xFFFFFF);
            M5.Display.drawRoundRect(x, y, cell_w, cell_h, 5, 0xFFFFFF);
        }
    }

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(4, H - 10);
    M5.Display.print("A=next B=flip A+B=exit");
    k85_draw_battery_icon();
}

void k85_run_memory_match(void) {
    uint8_t vals[K85_MEM_CARDS];
    for (int i = 0; i < K85_MEM_CARDS; i++) vals[i] = (uint8_t)(i / 2);
    for (int i = K85_MEM_CARDS - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint8_t t = vals[i]; vals[i] = vals[j]; vals[j] = t;
    }

    bool revealed[K85_MEM_CARDS] = { false };
    bool matched[K85_MEM_CARDS] = { false };
    int sel = 0;
    int moves = 0;
    int first = -1;
    int pairs_found = 0;

    draw_memory(vals, revealed, matched, sel, moves);

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }

        bool redraw = false;

        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            int n = sel;
            for (int step = 0; step < K85_MEM_CARDS; step++) {
                n = (n + 1) % K85_MEM_CARDS;
                if (!matched[n]) break;
            }
            sel = n;
            redraw = true;
        }

        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            if (!matched[sel] && !revealed[sel]) {
                revealed[sel] = true;
                k85_play_tone(700, 40);
                if (first < 0) {
                    first = sel;
                } else {
                    moves++;
                    draw_memory(vals, revealed, matched, sel, moves);
                    if (vals[first] == vals[sel]) {
                        matched[first] = true;
                        matched[sel] = true;
                        revealed[first] = false;
                        revealed[sel] = false;
                        pairs_found++;
                        k85_play_tone(1000, 80);
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(700));
                        revealed[first] = false;
                        revealed[sel] = false;
                        k85_play_tone(250, 80);
                    }
                    first = -1;
                }
            }
            redraw = true;
        }

        if (pairs_found >= K85_MEM_PAIRS) {
            draw_memory(vals, revealed, matched, -1, moves);
            k85_set_low_score_if_better("memory", moves);
            char msg[96];
            snprintf(msg, sizeof(msg), "Done!\n%d moves\nBest: %d\nA+B=back",
                     moves, k85_get_high_score("memory"));
            k85_show_message(msg);
            while (true) {
                k85_input_update();
                if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
                vTaskDelay(pdMS_TO_TICKS(30));
            }
        }

        if (redraw) draw_memory(vals, revealed, matched, sel, moves);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
