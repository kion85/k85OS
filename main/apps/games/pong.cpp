#include "pong.h"
#include "theme.h"
#include "battery.h"
#include "input.h"
#include "common.h"
#include "config.h"
#include "sound.h"

#include "M5Unified.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

#define K85_PONG_WIN_SCORE 5

static void draw_pong(int W, int H, uint32_t bg, uint32_t fg,
                       float player_y, float ai_y, int player_x, int ai_x,
                       int paddle_w, int paddle_h,
                       float ball_x, float ball_y, int ball_size,
                       int player_score, int ai_score) {
    M5.Display.fillScreen(bg);

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(fg, bg);
    char score_buf[8];
    snprintf(score_buf, sizeof(score_buf), "%d", player_score);
    M5.Display.setCursor(W / 4 - 6, 4);
    M5.Display.print(score_buf);
    snprintf(score_buf, sizeof(score_buf), "%d", ai_score);
    M5.Display.setCursor(3 * W / 4 - 6, 4);
    M5.Display.print(score_buf);

    for (int y = 0; y < H; y += 8) {
        M5.Display.fillRect(W / 2 - 1, y, 2, 4, 0x555555);
    }

    M5.Display.fillRect(player_x, (int)player_y, paddle_w, paddle_h, fg);
    M5.Display.fillRect(ai_x, (int)ai_y, paddle_w, paddle_h, fg);
    M5.Display.fillRect((int)ball_x - ball_size / 2, (int)ball_y - ball_size / 2, ball_size, ball_size, fg);

    k85_draw_battery_icon();
}

void k85_run_pong(void) {
    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    const int paddle_w = 4;
    const int paddle_h = 24;
    const float paddle_speed = 4.0f;
    const int player_x = 6;
    const int ai_x = W - 6 - paddle_w;
    const int ball_size = 4;

    float player_y = (H - paddle_h) / 2.0f;
    float ai_y = (H - paddle_h) / 2.0f;
    float ball_x, ball_y, ball_vx, ball_vy;

    int player_score = 0;
    int ai_score = 0;

    auto serve = [&](int direction) {
        ball_x = W / 2.0f;
        ball_y = H / 2.0f;
        ball_vx = 3.0f * direction;
        ball_vy = (float)((int)(esp_random() % 5) - 2);
        if (ball_vy == 0.0f) ball_vy = 1.0f;
    };
    serve((esp_random() % 2) ? 1 : -1);

    draw_pong(W, H, bg, fg, player_y, ai_y, player_x, ai_x, paddle_w, paddle_h,
              ball_x, ball_y, ball_size, player_score, ai_score);

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }

        if (k85_btn_a_is_down()) player_y -= paddle_speed;
        if (k85_btn_b_is_down()) player_y += paddle_speed;
        if (player_y < 0) player_y = 0;
        if (player_y > H - paddle_h) player_y = (float)(H - paddle_h);

        // Простой ИИ - едет к мячу с ограниченной скоростью (не идеально точен).
        float ai_target = ball_y - paddle_h / 2.0f;
        if (ai_y < ai_target) ai_y += paddle_speed * 0.7f;
        else if (ai_y > ai_target) ai_y -= paddle_speed * 0.7f;
        if (ai_y < 0) ai_y = 0;
        if (ai_y > H - paddle_h) ai_y = (float)(H - paddle_h);

        ball_x += ball_vx;
        ball_y += ball_vy;

        if (ball_y <= 0) { ball_y = 0; ball_vy = -ball_vy; }
        if (ball_y >= H - ball_size) { ball_y = (float)(H - ball_size); ball_vy = -ball_vy; }

        if (ball_vx < 0 && ball_x <= player_x + paddle_w && ball_x >= player_x &&
            ball_y + ball_size >= player_y && ball_y <= player_y + paddle_h) {
            ball_x = (float)(player_x + paddle_w);
            ball_vx = -ball_vx * 1.05f; // с каждым отбитым мячом чуть быстрее
            float rel = ((ball_y - player_y) / paddle_h) - 0.5f;
            ball_vy += rel * 3.0f;
            k85_play_tone(600, 40);
        }
        if (ball_vx > 0 && ball_x + ball_size >= ai_x && ball_x <= ai_x + paddle_w &&
            ball_y + ball_size >= ai_y && ball_y <= ai_y + paddle_h) {
            ball_x = (float)(ai_x - ball_size);
            ball_vx = -ball_vx * 1.05f;
            float rel = ((ball_y - ai_y) / paddle_h) - 0.5f;
            ball_vy += rel * 3.0f;
            k85_play_tone(400, 40);
        }

        if (ball_x < 0) {
            ai_score++;
            k85_play_tone(200, 150);
            serve(-1);
        } else if (ball_x > W) {
            player_score++;
            k85_play_tone(900, 150);
            serve(1);
        }

        draw_pong(W, H, bg, fg, player_y, ai_y, player_x, ai_x, paddle_w, paddle_h,
                  ball_x, ball_y, ball_size, player_score, ai_score);

        if (player_score >= K85_PONG_WIN_SCORE || ai_score >= K85_PONG_WIN_SCORE) {
            bool won = player_score > ai_score;
            k85_set_high_score_if_better("pong", player_score);
            char msg[64];
            snprintf(msg, sizeof(msg), "%s!\n%d - %d\nA+B=back", won ? "You win" : "AI wins", player_score, ai_score);
            k85_show_message(msg);
            while (true) {
                k85_input_update();
                if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
                vTaskDelay(pdMS_TO_TICKS(30));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
