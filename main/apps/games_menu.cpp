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

#include <cstring>

#define K85_GAMES_ITEM_COUNT 8

static const char *k85_games_labels[K85_GAMES_ITEM_COUNT] = {
    "Snake", "Tetris", "2048", "Race", "Reaction", "Flappy", "Download", "Back",
};

// Иконки для главного меню Games.
static void draw_games_icon(int cx, int cy, int r, const char *name, uint32_t col, uint32_t bg_col) {
    auto &d = M5.Display;
    if (!strcmp(name, "Snake")) {
        d.drawLine(cx - r, cy - r/2, cx - r/3, cy - r/2, col);
        d.drawLine(cx - r/3, cy - r/2, cx - r/3, cy, col);
        d.drawLine(cx - r/3, cy, cx + r/3, cy, col);
        d.drawLine(cx + r/3, cy, cx + r/3, cy + r/2, col);
        d.drawLine(cx + r/3, cy + r/2, cx + r, cy + r/2, col);
        d.fillCircle(cx + r, cy + r/2, 2, col);
    } else if (!strcmp(name, "Tetris")) {
        // L-тетромино из квадратиков.
        int s = r / 2;
        d.fillRect(cx - s, cy - s, s, s, col);
        d.fillRect(cx - s, cy, s, s, col);
        d.fillRect(cx - s, cy + s, s, s, col);
        d.fillRect(cx, cy + s, s, s, col);
    } else if (!strcmp(name, "2048")) {
        int s = r - 2;
        d.drawRect(cx - s, cy - s, s, s, col);
        d.drawRect(cx, cy - s, s, s, col);
        d.drawRect(cx - s, cy, s, s, col);
        d.fillRect(cx, cy, s, s, col);
    } else if (!strcmp(name, "Race")) {
        // Чекерный флаг.
        for (int row = 0; row < 4; row++) {
            for (int c = 0; c < 3; c++) {
                if ((row + c) % 2 == 0) {
                    d.fillRect(cx - r/2 + c * (r/3), cy - r + row * (r/2), r/3, r/2, col);
                }
            }
        }
    } else if (!strcmp(name, "Reaction")) {
        // Молния.
        d.fillTriangle(cx + r/4, cy - r, cx - r/2, cy + r/6, cx, cy + r/6, col);
        d.fillTriangle(cx, cy + r/6, cx - r/4, cy + r, cx + r/2, cy - r/6, col);
    } else if (!strcmp(name, "Flappy")) {
        d.fillCircle(cx, cy, r/2, col);
        d.fillTriangle(cx - r/2, cy, cx - r, cy - r/4, cx - r, cy + r/4, col);
        d.fillCircle(cx + r/4, cy - r/4, 2, bg_col);
    } else if (!strcmp(name, "Download")) {
        d.drawLine(cx, cy - r, cx, cy + r/3, col);
        d.fillTriangle(cx - r/3, cy, cx + r/3, cy, cx, cy + r/2, col);
        d.drawFastHLine(cx - r/2, cy + r, r, col);
    } else if (!strcmp(name, "Back")) {
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy, col);
        d.drawLine(cx - r/2, cy, cx + r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy, cx + r, cy, col);
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
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
    static char sig_urls[K85_APPREPO_MAX_ASSETS][256];
    int count = 0;
    if (!k85_apprepo_fetch_app_list(names, urls, sig_urls, K85_APPREPO_MAX_ASSETS, &count) || count == 0) {
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

    // Имена загружаемых приложений заранее не известны (приходят из
    // репозитория), поэтому здесь без icon_fn - используется дефолтная
    // "буква в кружке" из list_menu.cpp.
    int idx = k85_run_list_menu("DOWNLOAD", items, count + 1, nullptr);
    if (idx < 0 || idx >= count) return;

    k85_show_message("Downloading &\nflashing...\nThis may take a while");
    bool ok = k85_fwflash_from_url(urls[idx], sig_urls[idx], nullptr);

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
    while (true) {
        int idx = k85_run_list_menu("GAMES", k85_games_labels, K85_GAMES_ITEM_COUNT, nullptr, draw_games_icon);
        if (idx < 0 || idx == K85_GAMES_ITEM_COUNT - 1) return;
        switch (idx) {
            case 0: k85_run_snake(); break;
            case 1: k85_run_tetris(); break;
            case 2: k85_run_2048(); break;
            case 3: k85_run_race(); break;
            case 4: k85_run_reaction(); break;
            case 5: k85_run_flappy(); break;
            case 6: run_download_menu(); break;
            default: break;
        }
    }
}