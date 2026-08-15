#include "race.h"
#include "config.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "common.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <cstdio>
#include "esp_random.h"

#define K85_RACE_LANES 2
#define K85_RACE_MAX_ENEMIES 6

struct K85RaceEnemy {
    bool active;
    int lane;
    float y;
};

static int k85_race_rand_int(int n) {
    if (n <= 0) return 0;
    return (int)(esp_random() % (uint32_t)n);
}

// Рисует машинку: кузов + лобовое стекло + 4 колеса
static void draw_car(int cx, int y, int w, int h, uint32_t body_color) {
    int x = cx - w / 2;
    uint32_t window_color = 0x88CCFF;
    uint32_t wheel_color = 0x111111;

    // Колёса (по бокам, чуть выступают за кузов)
    int wheel_w = 3;
    int wheel_h = h / 3;
    M5.Display.fillRect(x - wheel_w + 1, y + 2, wheel_w, wheel_h, wheel_color);
    M5.Display.fillRect(x + w - 1, y + 2, wheel_w, wheel_h, wheel_color);
    M5.Display.fillRect(x - wheel_w + 1, y + h - wheel_h - 2, wheel_w, wheel_h, wheel_color);
    M5.Display.fillRect(x + w - 1, y + h - wheel_h - 2, wheel_w, wheel_h, wheel_color);

    // Кузов
    M5.Display.fillRoundRect(x, y, w, h, 3, body_color);

    // Лобовое стекло (полоска сверху кузова)
    int win_h = h / 3;
    M5.Display.fillRoundRect(x + 2, y + 2, w - 4, win_h, 2, window_color);
}

void k85_run_race(void) {
    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    const int road_margin = 10;
    const int road_w = W - road_margin * 2;
    const int lane_w = road_w / K85_RACE_LANES;
    const int car_w = lane_w - 12;
    const int car_h = 16;
    const int player_y = H - 26;

    auto lane_cx = [&](int lane) -> int {
        return road_margin + lane * lane_w + lane_w / 2;
    };

    int player_lane = 0;
    K85RaceEnemy enemies[K85_RACE_MAX_ENEMIES];
    for (int i = 0; i < K85_RACE_MAX_ENEMIES; i++) enemies[i].active = false;

    float speed = 60.0f;
    float spawn_timer = 0.0f;
    float spawn_interval = 1.0f;
    float survive_time = 0.0f;
    bool game_over = false;

    uint32_t last_us = (uint32_t)esp_timer_get_time();

    static const uint32_t enemy_colors[] = {0xFF3333, 0xFF9933, 0xCC33FF, 0xFFDD33};

    auto draw = [&](uint32_t score) {
        M5.Display.fillScreen(bg);
        M5.Display.drawFastVLine(road_margin, 0, H, 0x555555);
        M5.Display.drawFastVLine(road_margin + road_w, 0, H, 0x555555);
        for (int l = 1; l < K85_RACE_LANES; l++) {
            int x = road_margin + l * lane_w;
            for (int y = 0; y < H; y += 12) {
                M5.Display.drawFastVLine(x, y, 6, 0x333333);
            }
        }
        for (int i = 0; i < K85_RACE_MAX_ENEMIES; i++) {
            if (!enemies[i].active) continue;
            int cx = lane_cx(enemies[i].lane);
            draw_car(cx, (int)enemies[i].y, car_w, car_h, enemy_colors[i % 4]);
        }
        int pcx = lane_cx(player_lane);
        draw_car(pcx, player_y, car_w, car_h, 0x33FF66);

        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 4);
        M5.Display.printf("Race score:%lu", (unsigned long)score);
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, H - 12);
        M5.Display.print("A=lane A+B=exit");
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

    draw(0);

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }

        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            player_lane = (player_lane + 1) % K85_RACE_LANES;
        }

        uint32_t now_us = (uint32_t)esp_timer_get_time();
        float dt = (now_us - last_us) / 1000000.0f;
        last_us = now_us;
        if (dt > 0.1f) dt = 0.1f;

        speed += dt * 4.0f;
        if (spawn_interval > 0.35f) spawn_interval -= dt * 0.01f;
        survive_time += dt;

        spawn_timer += dt;
        if (spawn_timer >= spawn_interval) {
            spawn_timer = 0.0f;
            for (int i = 0; i < K85_RACE_MAX_ENEMIES; i++) {
                if (!enemies[i].active) {
                    enemies[i].active = true;
                    enemies[i].lane = k85_race_rand_int(K85_RACE_LANES);
                    enemies[i].y = -car_h;
                    break;
                }
            }
        }

        for (int i = 0; i < K85_RACE_MAX_ENEMIES; i++) {
            if (!enemies[i].active) continue;
            enemies[i].y += speed * dt;
            if (enemies[i].y > H) {
                enemies[i].active = false;
                continue;
            }
            if (enemies[i].lane == player_lane) {
                bool overlap_y = (enemies[i].y + car_h > player_y) && (enemies[i].y < player_y + car_h);
                if (overlap_y) game_over = true;
            }
        }

        uint32_t score = (uint32_t)(survive_time * 10.0f);

        if (game_over) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Score:%lu", (unsigned long)score);
            game_over_screen(buf);
            return;
        }

        draw(score);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}