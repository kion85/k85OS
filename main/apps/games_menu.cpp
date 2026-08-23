#include "games_menu.h"
#include "list_menu.h"
#include "games/snake.h"
#include "games/tetris.h"
#include "games/game2048.h"
#include "games/race.h"
#include "games/reaction.h"
#include "games/flappy.h"
#include "../net/app_repo.h"
#include "../net/firmware_flash.h"
#include "../net/wifi.h"
#include "theme.h"
#include "battery.h"
#include "input.h"
#include "common.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define K85_GAMES_ITEM_COUNT 8

static const char *k85_games_labels[K85_GAMES_ITEM_COUNT] = {
    "Snake", "Tetris", "2048", "Race", "Reaction", "Flappy", "Download", "Back",
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

static void run_download_menu(void) {
    if (!k85_wifi_is_connected()) {
        k85_show_message("WiFi not connected\nA+B=back");
        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    k85_show_message("Fetching app list...");
    static char names[K85_APPREPO_MAX_ASSETS][64];
    static char urls[K85_APPREPO_MAX_ASSETS][256];
    int count = 0;
    if (!k85_apprepo_fetch_app_list(names, urls, K85_APPREPO_MAX_ASSETS, &count) || count == 0) {
        k85_show_message("No apps found\nin apps_k85os 'app'\nrelease\nA+B=back");
        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    const char *items[K85_APPREPO_MAX_ASSETS + 1];
    for (int i = 0; i < count; i++) items[i] = names[i];
    items[count] = "Back";

    int idx = k85_run_list_menu("DOWNLOAD", items, count + 1, nullptr);
    if (idx < 0 || idx >= count) return;

    k85_show_message("Downloading &\nflashing...\nThis may take a while");
    bool ok = k85_fwflash_from_url(urls[idx], nullptr);

    k85_show_message(ok
        ? "Flashed to free slot!\nActivate via device\nGRUB -> Alt Firmware\nA+B=back"
        : "Download/flash failed\nA+B=back");
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
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
                case 6: run_download_menu(); break;
                default: return; // Back
            }
            games_draw();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

