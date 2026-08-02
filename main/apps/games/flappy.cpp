#include "flappy.h"
#include "config.h"
#include "theme.h"
#include "input.h"
#include "power.h"
#include "common.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <cstdio>
#include <cstring>

#define K85_FLAPPY_MAX_PIPES 6

struct K85FlappyPipe {
    bool active;
    float x;
    float gap_y;
    bool scored;
};

static uint32_t k85_flappy_millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Возвращает true если нужно начать заново (B на экране game over)
static bool run_flappy_once(void) {
    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    float player_x = 30;
    float player_y = H / 2.0f;
    float velocity = 0.0f;
    const float gravity = 0.6f;
    const float jump_power = -4.5f;
    const int pipe_w = 20;
    const int gap_h = 45;
    const float pipe_speed = 3.0f;

    K85FlappyPipe pipes[K85_FLAPPY_MAX_PIPES];
    memset(pipes, 0, sizeof(pipes));
    pipes[0].active = true;
    pipes[0].x = (float)W;
    pipes[0].gap_y = H / 2.0f;
    pipes[0].scored = false;

    int score = 0;
    const int frame_delay = 30;
    uint32_t last_spawn = k85_flappy_millis();
    const uint32_t spawn_interval = 1500;
    bool game_over = false;
    bool recorded = false;

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            if (!recorded) k85_set_high_score_if_better("flappy", score);
            return false;
        }

        if (game_over) {
            if (!recorded) {
                k85_set_high_score_if_better("flappy", score);
                recorded = true;
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
            continue;
        }

        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            velocity = jump_power;
        }
        velocity += gravity;
        player_y += velocity;

        uint32_t now = k85_flappy_millis();
        if (now - last_spawn > spawn_interval) {
            for (int i = 0; i < K85_FLAPPY_MAX_PIPES; i++) {
                if (!pipes[i].active) {
                    pipes[i].active = true;
                    pipes[i].x = (float)W;
                    pipes[i].gap_y = 25.0f + (float)(now % (uint32_t)(H - 50));
                    pipes[i].scored = false;
                    break;
                }
            }
            last_spawn = now;
        }

        for (int i = 0; i < K85_FLAPPY_MAX_PIPES; i++) {
            if (!pipes[i].active) continue;
            pipes[i].x -= pipe_speed;
            if (pipes[i].x + pipe_w < player_x && !pipes[i].scored) {
                pipes[i].scored = true;
                score++;
            }
            if (pipes[i].x <= -pipe_w) {
                pipes[i].active = false;
            }
        }

        M5.Display.fillScreen(bg);
        M5.Display.fillCircle((int)player_x, (int)player_y, 6, 0xFFFF00);

        for (int i = 0; i < K85_FLAPPY_MAX_PIPES; i++) {
            if (!pipes[i].active) continue;
            int top_h = (int)(pipes[i].gap_y - gap_h / 2);
            int bottom_y = (int)(pipes[i].gap_y + gap_h / 2);
            M5.Display.fillRect((int)pipes[i].x, 0, pipe_w, top_h, 0x00FF00);
            M5.Display.fillRect((int)pipes[i].x, bottom_y, pipe_w, H - bottom_y, 0x00FF00);

            if (player_x + 6 > pipes[i].x && player_x - 6 < pipes[i].x + pipe_w) {
                if (player_y - 6 < top_h || player_y + 6 > bottom_y) {
                    game_over = true;
                }
            }
        }

        if (player_y < 0 || player_y > H) {
            game_over = true;
        }

        M5.Display.setTextSize(2);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 4);
        M5.Display.printf("%d", score);

        if (game_over) {
            char buf[40];
            snprintf(buf, sizeof(buf), "Score: %d B=retry", score);
            M5.Display.fillScreen(bg);
            M5.Display.setTextSize(2);
            M5.Display.setTextColor(0xFF0000, bg);
            M5.Display.setCursor(20, H / 2 - 20);
            M5.Display.print("GAME OVER");
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(fg, bg);
            M5.Display.setCursor(20, H / 2);
            M5.Display.print(buf);
            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.setCursor(20, H / 2 + 15);
            M5.Display.print("A+B hold = exit");
            while (true) {
                k85_input_update();
                if (k85_ab_held(500)) {
                    k85_wait_ab_release();
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(30));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(frame_delay));
    }
}

void k85_run_flappy(void) {
    while (run_flappy_once()) {
        // retry
    }
}

