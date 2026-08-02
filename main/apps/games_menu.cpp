#include "games_menu.h"
#include "games/snake.h"
#include "games/tetris.h"
#include "games/game2048.h"
#include "games/race.h"
#include "games/reaction.h"
#include "games/flappy.h"
#include "theme.h"
#include "battery.h"
#include "input.h"
#include "common.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define K85_GAMES_ITEM_COUNT 7

static const char *k85_games_labels[K85_GAMES_ITEM_COUNT] = {
    "Snake", "Tetris", "2048", "Race", "Reaction", "Flappy", "Back",
};

static int s_selected = 0;

static void games_draw(void) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(fg, bg);
    M5.Display.print("Games");

    int y = 20;
    for (int i = 0; i < K85_GAMES_ITEM_COUNT; i++) {
        bool sel = (i == s_selected);
        M5.Display.setCursor(6, y);
        M5.Display.setTextColor(sel ? accent : fg, bg);
        M5.Display.print(sel ? "> " : "  ");
        M5.Display.print(k85_games_labels[i]);
        y += 14;
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(6, y + 6);
    M5.Display.print("A=next B=select A+B=back");
}

void k85_run_games_menu(void) {
    s_selected = 0;
    games_draw();
    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }

        if (k85_btn_a_pressed()) {
            s_selected = (s_selected + 1) % K85_GAMES_ITEM_COUNT;
            games_draw();
        }

        if (k85_btn_b_pressed()) {
            switch (s_selected) {
                case 0: k85_run_snake(); break;
                case 1: k85_run_tetris(); break;
                case 2: k85_run_2048(); break;
                case 3: k85_run_race(); break;
                case 4: k85_run_reaction(); break;
                case 5: k85_run_flappy(); break;
                default: return; // Back
            }
            games_draw();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

