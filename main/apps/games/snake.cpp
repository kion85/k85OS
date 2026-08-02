#include "snake.h"
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
#include <cstdlib>
#include "esp_random.h"

#define K85_SNAKE_MAX_LEN 512

static int k85_snake_rand_int(int n) {
    if (n <= 0) return 0;
    return (int)(esp_random() % (uint32_t)n);
}

// Аналог run_snake() из MicroPython-версии k85OS (портировано 1:1: тайлы 8px,
// интервал хода 200мс, управление наклоном через акселерометр, game over экран)
void k85_run_snake(void) {
    const int cell = 8;
    const int oy = 16;
    int W = M5.Display.width();
    int H = M5.Display.height();
    int cols = W / cell;
    int rows = (H - 16) / cell;

    int snake_x[K85_SNAKE_MAX_LEN];
    int snake_y[K85_SNAKE_MAX_LEN];
    int snake_len = 1;
    snake_x[0] = cols / 2;
    snake_y[0] = rows / 2;

    int dir_x = 1, dir_y = 0;

    int apple_x = 0, apple_y = 0;

    auto snake_contains = [&](int x, int y) -> bool {
        for (int i = 0; i < snake_len; i++) {
            if (snake_x[i] == x && snake_y[i] == y) return true;
        }
        return false;
    };

    auto place_apple = [&]() {
        while (true) {
            int px = k85_snake_rand_int(cols);
            int py = k85_snake_rand_int(rows);
            if (!snake_contains(px, py)) {
                apple_x = px;
                apple_y = py;
                return;
            }
        }
    };

    place_apple();
    int score = 0;
    uint32_t last_move = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t move_interval = 200;
    bool game_over = false;

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    auto draw = [&]() {
        M5.Display.fillScreen(bg);
        M5.Display.fillRect(apple_x * cell, oy + apple_y * cell, cell - 1, cell - 1, 0xFF0000);
        for (int i = 0; i < snake_len; i++) {
            uint32_t col = (i > 0) ? 0x00FF00 : 0xFFFF00;
            M5.Display.fillRect(snake_x[i] * cell, oy + snake_y[i] * cell, cell - 1, cell - 1, col);
        }
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.printf("Snake score:%d", score);
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, H - 12);
        M5.Display.print("tilt A+B=exit");
        k85_draw_battery_icon();
    };

    auto game_over_screen = [&](const char *score_text) {
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
        M5.Display.print("A+B hold = exit");
        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) {
                k85_wait_ab_release();
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    };

    draw();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            k85_set_high_score_if_better("snake", score);
            return;
        }
        if (game_over) {
            if (k85_btn_a_pressed() || k85_btn_b_pressed()) {
                k85_wake_screen();
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        float ax = 0, ay = 0, az = 0;
        M5.Imu.getAccel(&ax, &ay, &az);

        if (fabsf(ax) > fabsf(ay)) {
            if (ax > 0.3f) { dir_x = 1; dir_y = 0; }
            else if (ax < -0.3f) { dir_x = -1; dir_y = 0; }
        } else {
            if (ay > 0.3f) { dir_x = 0; dir_y = 1; }
            else if (ay < -0.3f) { dir_x = 0; dir_y = -1; }
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_move > move_interval) {
            last_move = now;
            int hx = snake_x[0], hy = snake_y[0];
            int nx = hx + dir_x, ny = hy + dir_y;

            if (nx < 0 || nx >= cols || ny < 0 || ny >= rows || snake_contains(nx, ny)) {
                game_over = true;
                k85_set_high_score_if_better("snake", score);
                char buf[32];
                snprintf(buf, sizeof(buf), "Score:%d", score);
                game_over_screen(buf);
            } else {
                if (snake_len < K85_SNAKE_MAX_LEN) {
                    for (int i = snake_len; i > 0; i--) {
                        snake_x[i] = snake_x[i - 1];
                        snake_y[i] = snake_y[i - 1];
                    }
                    snake_x[0] = nx;
                    snake_y[0] = ny;
                    snake_len++;
                }
                if (nx == apple_x && ny == apple_y) {
                    score++;
                    place_apple();
                } else if (snake_len > 1) {
                    snake_len--;
                }
                draw();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
