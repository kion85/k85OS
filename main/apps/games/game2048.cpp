#include "game2048.h"
#include "config.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "common.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include "esp_random.h"

#define K85_2048_SIZE 4

static int k85_2048_rand_int(int n) {
    if (n <= 0) return 0;
    return (int)(esp_random() % (uint32_t)n);
}

static uint32_t tile_color(int v) {
    switch (v) {
        case 0:    return 0x1a1a1a;
        case 2:    return 0xeee4da;
        case 4:    return 0xede0c8;
        case 8:    return 0xf2b179;
        case 16:   return 0xf59563;
        case 32:   return 0xf67c5f;
        case 64:   return 0xf65e3b;
        case 128:  return 0xedcf72;
        case 256:  return 0xedcc61;
        case 512:  return 0xedc850;
        case 1024: return 0xedc53f;
        case 2048: return 0xedc22e;
        default:   return 0xFFFFFF;
    }
}

// Возвращает true если нужно начать заново (игрок нажал B на game over экране)
static bool run_2048_once(void) {
    int grid[K85_2048_SIZE][K85_2048_SIZE];
    memset(grid, 0, sizeof(grid));

    auto add_random_tile = [&]() -> bool {
        int empty_r[K85_2048_SIZE * K85_2048_SIZE];
        int empty_c[K85_2048_SIZE * K85_2048_SIZE];
        int n = 0;
        for (int r = 0; r < K85_2048_SIZE; r++)
            for (int c = 0; c < K85_2048_SIZE; c++)
                if (grid[r][c] == 0) { empty_r[n] = r; empty_c[n] = c; n++; }
        if (n == 0) return false;
        int idx = k85_2048_rand_int(n);
        grid[empty_r[idx]][empty_c[idx]] = (k85_2048_rand_int(10) == 0) ? 4 : 2;
        return true;
    };

    auto compress_merge = [&](int *line, int *out_line, int *gained) {
        int vals[K85_2048_SIZE];
        int vn = 0;
        for (int i = 0; i < K85_2048_SIZE; i++) if (line[i] != 0) vals[vn++] = line[i];
        int merged[K85_2048_SIZE];
        int mn = 0;
        *gained = 0;
        int i = 0;
        while (i < vn) {
            if (i + 1 < vn && vals[i] == vals[i + 1]) {
                merged[mn++] = vals[i] * 2;
                *gained += vals[i] * 2;
                i += 2;
            } else {
                merged[mn++] = vals[i];
                i += 1;
            }
        }
        for (int k = 0; k < K85_2048_SIZE; k++) out_line[k] = (k < mn) ? merged[k] : 0;
    };

    // direction: 0=up 1=right 2=down 3=left (как в оригинале)
    auto move_grid = [&](int direction, int *gained_total) -> bool {
        bool changed = false;
        *gained_total = 0;
        if (direction == 1 || direction == 3) {
            for (int r = 0; r < K85_2048_SIZE; r++) {
                int line[K85_2048_SIZE], newline[K85_2048_SIZE];
                for (int c = 0; c < K85_2048_SIZE; c++) line[c] = grid[r][c];
                if (direction == 1) {
                    int rev[K85_2048_SIZE];
                    for (int c = 0; c < K85_2048_SIZE; c++) rev[c] = line[K85_2048_SIZE - 1 - c];
                    memcpy(line, rev, sizeof(line));
                }
                int gained;
                compress_merge(line, newline, &gained);
                if (direction == 1) {
                    int rev[K85_2048_SIZE];
                    for (int c = 0; c < K85_2048_SIZE; c++) rev[c] = newline[K85_2048_SIZE - 1 - c];
                    memcpy(newline, rev, sizeof(newline));
                }
                for (int c = 0; c < K85_2048_SIZE; c++) {
                    if (grid[r][c] != newline[c]) changed = true;
                    grid[r][c] = newline[c];
                }
                *gained_total += gained;
            }
        } else {
            for (int c = 0; c < K85_2048_SIZE; c++) {
                int col[K85_2048_SIZE], newcol[K85_2048_SIZE];
                for (int r = 0; r < K85_2048_SIZE; r++) col[r] = grid[r][c];
                if (direction == 2) {
                    int rev[K85_2048_SIZE];
                    for (int r = 0; r < K85_2048_SIZE; r++) rev[r] = col[K85_2048_SIZE - 1 - r];
                    memcpy(col, rev, sizeof(col));
                }
                int gained;
                compress_merge(col, newcol, &gained);
                if (direction == 2) {
                    int rev[K85_2048_SIZE];
                    for (int r = 0; r < K85_2048_SIZE; r++) rev[r] = newcol[K85_2048_SIZE - 1 - r];
                    memcpy(newcol, rev, sizeof(newcol));
                }
                for (int r = 0; r < K85_2048_SIZE; r++) {
                    if (grid[r][c] != newcol[r]) changed = true;
                    grid[r][c] = newcol[r];
                }
                *gained_total += gained;
            }
        }
        return changed;
    };

    auto has_moves = [&]() -> bool {
        for (int r = 0; r < K85_2048_SIZE; r++) {
            for (int c = 0; c < K85_2048_SIZE; c++) {
                if (grid[r][c] == 0) return true;
                if (c + 1 < K85_2048_SIZE && grid[r][c] == grid[r][c + 1]) return true;
                if (r + 1 < K85_2048_SIZE && grid[r][c] == grid[r + 1][c]) return true;
            }
        }
        return false;
    };

    int score = 0;
    int direction = 0;

    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    auto draw = [&]() {
        M5.Display.fillScreen(bg);
        const int margin = 4;
        int cell = (W - margin * (K85_2048_SIZE + 1)) / K85_2048_SIZE;
        int cell2 = (H - 40 - margin * (K85_2048_SIZE + 1)) / K85_2048_SIZE;
        if (cell2 < cell) cell = cell2;
        int gx = (W - (cell * K85_2048_SIZE + margin * (K85_2048_SIZE + 1))) / 2;
        int gy = 20;

        for (int r = 0; r < K85_2048_SIZE; r++) {
            for (int c = 0; c < K85_2048_SIZE; c++) {
                int v = grid[r][c];
                int x = gx + margin + c * (cell + margin);
                int y = gy + margin + r * (cell + margin);
                uint32_t col = tile_color(v);
                M5.Display.fillRect(x, y, cell, cell, col);
                if (v) {
                    M5.Display.setTextSize(1);
                    M5.Display.setTextColor(v <= 8 ? 0x000000 : 0xFFFFFF, col);
                    char txt[8];
                    snprintf(txt, sizeof(txt), "%d", v);
                    int len = (int)strlen(txt);
                    M5.Display.setCursor(x + cell / 2 - len * 3, y + cell / 2 - 4);
                    M5.Display.print(txt);
                }
            }
        }
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 4);
        M5.Display.printf("2048 score:%d", score);
        const char *arrows[4] = {"^", ">", "v", "<"};
        M5.Display.setCursor(W - 16, 4);
        M5.Display.print(arrows[direction]);
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, H - 12);
        M5.Display.print("A=dir B=move A+B=exit");
        k85_draw_battery_icon();
    };

    auto game_over_screen = [&](const char *score_text) -> bool {
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(0xFF0000, bg);
        M5.Display.setCursor(20, H / 2 - 20);
        M5.Display.print("GAME OVER");
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(20, H / 2);
        M5.Display.print(score_text);
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(20, H / 2 + 15);
        M5.Display.print("A=exit B=retry A+B=exit");
        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) {
                k85_wait_ab_release();
                return false;
            }
            if (k85_btn_b_pressed()) {
                k85_wake_screen();
                return true;
            }
            if (k85_btn_a_pressed()) {
                k85_wake_screen();
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    };

    add_random_tile();
    add_random_tile();
    draw();

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            k85_set_high_score_if_better("2048", score);
            return false;
        }
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            direction = (direction + 1) % 4;
            draw();
        }
        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            int gained;
            bool changed = move_grid(direction, &gained);
            score += gained;
            if (changed) add_random_tile();
            if (!has_moves()) {
                k85_set_high_score_if_better("2048", score);
                char buf[32];
                snprintf(buf, sizeof(buf), "Score:%d", score);
                return game_over_screen(buf);
            } else {
                draw();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_run_2048(void) {
    while (run_2048_once()) {
        // retry по нажатию B на экране game over
    }
}
