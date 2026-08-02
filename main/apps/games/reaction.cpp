#include "reaction.h"
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

static uint32_t k85_reaction_millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void k85_run_reaction(void) {
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    while (true) {
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(10, H / 2 - 20);
        M5.Display.print("Reaction test");
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(10, H / 2 + 10);
        M5.Display.print("B=play  A+B=exit");
        vTaskDelay(pdMS_TO_TICKS(500));

        bool go_play = false;
        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) {
                k85_wait_ab_release();
                return;
            }
            if (k85_btn_a_pressed()) {
                k85_wake_screen();
                return;
            }
            if (k85_btn_b_pressed()) {
                k85_wake_screen();
                go_play = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        if (!go_play) return;

        M5.Display.fillScreen(0xFF0000);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(0xFFFFFF, 0xFF0000);
        M5.Display.setCursor(20, H / 2 - 8);
        M5.Display.print("Wait...");

        uint32_t delay_ms = 1000 + (k85_reaction_millis() % 2000);
        uint32_t wait_start = k85_reaction_millis();
        bool too_soon = false;

        while (k85_reaction_millis() - wait_start < delay_ms) {
            k85_input_update();
            if (k85_btn_b_pressed()) {
                too_soon = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (too_soon) {
            k85_show_message("Too soon!\nA+B=back");
            while (true) {
                k85_input_update();
                if (k85_ab_held(500)) {
                    k85_wait_ab_release();
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            continue;
        }

        M5.Display.fillScreen(0x00FF00);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(0x000000, 0x00FF00);
        M5.Display.setCursor(30, H / 2 - 8);
        M5.Display.print("PRESS B!");

        uint32_t react_start = k85_reaction_millis();
        bool exited = false;

        while (true) {
            k85_input_update();
            if (k85_btn_b_pressed()) {
                uint32_t reaction_ms = k85_reaction_millis() - react_start;
                k85_set_low_score_if_better("reaction", (int)reaction_ms);
                char buf[32];
                snprintf(buf, sizeof(buf), "%lu ms\nA+B=back", (unsigned long)reaction_ms);
                k85_show_message(buf);
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) {
                        k85_wait_ab_release();
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                break;
            }
            if (k85_ab_held(500)) {
                k85_wait_ab_release();
                exited = true;
                break;
            }
            if (k85_btn_a_pressed()) {
                k85_wake_screen();
                exited = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        if (exited) return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


