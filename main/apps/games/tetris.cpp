#include "tetris.h"
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
#include <vector>
#include "esp_random.h"
#include "esp_timer.h"

struct TCell { int r, c; };

// Те же 5 фигур, что и в оригинале (I/O/T/L/S — J и Z отсутствуют в исходнике, это не ошибка порта)
static const TCell TETRO_I[] = {{0,0},{0,1},{0,2},{0,3}};
static const TCell TETRO_O[] = {{0,0},{0,1},{1,0},{1,1}};
static const TCell TETRO_T[] = {{0,0},{0,1},{0,2},{1,1}};
static const TCell TETRO_L[] = {{0,0},{1,0},{2,0},{2,1}};
static const TCell TETRO_S[] = {{0,1},{0,2},{1,0},{1,1}};
static const TCell *TETROMINOES[5] = {TETRO_I, TETRO_O, TETRO_T, TETRO_L, TETRO_S};

static int64_t k85_ticks_ms() { return esp_timer_get_time() / 1000; }
static int k85_tetris_rand(int n) { return n <= 0 ? 0 : (int)(esp_random() % (uint32_t)n); }

static void rotate_piece(TCell cells[4]) {
    // (r,c) -> (c,-r), затем нормализация в неотрицательные координаты — как rotate_piece() в оригинале
    TCell tmp[4];
    for (int i = 0; i < 4; i++) tmp[i] = {cells[i].c, -cells[i].r};
    int minr = tmp[0].r, minc = tmp[0].c;
    for (int i = 1; i < 4; i++) { if (tmp[i].r < minr) minr = tmp[i].r; if (tmp[i].c < minc) minc = tmp[i].c; }
    for (int i = 0; i < 4; i++) cells[i] = {tmp[i].r - minr, tmp[i].c - minc};
}

void k85_run_tetris(void) {
    int W = M5.Display.width();
    int H = M5.Display.height();

    int cell = W / 10; if (cell < 6) cell = 6;
    int cols = W / cell; if (cols > 10) cols = 10; if (cols < 6) cols = 6;
    cell = W / cols;
    int rows = (H - 20) / cell; if (rows > 14) rows = 14;

    std::vector<std::vector<int>> board(rows, std::vector<int>(cols, 0));
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    TCell piece_cells[4];
    int piece_r, piece_c;

    auto new_piece = [&]() {
        int idx = k85_tetris_rand(5);
        memcpy(piece_cells, TETROMINOES[idx], sizeof(TCell) * 4);
        piece_r = 0;
        piece_c = cols / 2 - 1;
    };

    auto fits = [&](const TCell cells[4], int r0, int c0) -> bool {
        for (int i = 0; i < 4; i++) {
            int rr = r0 + cells[i].r, cc = c0 + cells[i].c;
            if (cc < 0 || cc >= cols || rr >= rows) return false;
            if (rr >= 0 && board[rr][cc]) return false;
        }
        return true;
    };

    auto lock_piece = [&]() -> int {
        for (int i = 0; i < 4; i++) {
            int rr = piece_r + piece_cells[i].r, cc = piece_c + piece_cells[i].c;
            if (rr >= 0 && rr < rows) board[rr][cc] = 1;
        }
        int cleared = 0;
        int r = rows - 1;
        while (r >= 0) {
            bool full = true;
            for (int c = 0; c < cols; c++) if (!board[r][c]) { full = false; break; }
            if (full) {
                board.erase(board.begin() + r);
                board.insert(board.begin(), std::vector<int>(cols, 0));
                cleared++;
            } else {
                r--;
            }
        }
        return cleared;
    };

    int score = 0;
    bool game_over = false;

    auto draw = [&]() {
        M5.Display.fillScreen(bg);
        int ox = (W - cols * cell) / 2;
        int oy = 16;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++)
                if (board[r][c]) M5.Display.fillRect(ox + c * cell, oy + r * cell, cell - 1, cell - 1, 0x00AAFF);
        for (int i = 0; i < 4; i++) {
            int rr = piece_r + piece_cells[i].r, cc = piece_c + piece_cells[i].c;
            if (rr >= 0) M5.Display.fillRect(ox + cc * cell, oy + rr * cell, cell - 1, cell - 1, 0xFFAA00);
        }
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.printf("Tetris score:%d", score);
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, H - 12);
        M5.Display.print("A=left B=rot A+B=exit");
        k85_draw_battery_icon();
    };

    new_piece();
    int64_t last_drop = k85_ticks_ms();
    const int drop_interval = 600;
    const int line_score[5] = {0, 10, 30, 60, 100};

    draw();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        if (game_over) {
            if (k85_btn_b_pressed()) {
                k85_wake_screen();
                k85_run_tetris();
                return;
            }
            if (k85_btn_a_pressed()) {
                k85_wake_screen();
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        int64_t now = k85_ticks_ms();
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            if (fits(piece_cells, piece_r, piece_c - 1)) piece_c--;
            draw();
        }
        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            TCell rotated[4];
            memcpy(rotated, piece_cells, sizeof(rotated));
            rotate_piece(rotated);
            if (fits(rotated, piece_r, piece_c)) {
                memcpy(piece_cells, rotated, sizeof(piece_cells));
            } else {
                while (fits(piece_cells, piece_r + 1, piece_c)) piece_r++;
            }
            draw();
        }
        if (now - last_drop > drop_interval) {
            last_drop = now;
            if (fits(piece_cells, piece_r + 1, piece_c)) {
                piece_r++;
            } else {
                int cleared = lock_piece();
                if (cleared > 4) cleared = 4;
                score += line_score[cleared];
                new_piece();
                if (!fits(piece_cells, piece_r, piece_c)) {
                    game_over = true;
                    k85_set_high_score_if_better("tetris", score);
                    char buf[32];
                    snprintf(buf, sizeof(buf), "Score:%d B=retry", score);
                    k85_show_message(buf);
                }
            }
            draw();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

