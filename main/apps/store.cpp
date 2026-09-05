#include "store.h"
#include "list_menu.h"
#include "common.h"
#include "input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "snake.h"
#include "tetris.h"
#include "../net/app_repo.h"
#include "../net/firmware_flash.h"
#include "../net/wifi.h"

static const char *STORE_CATEGORIES[] = {"Puzzle", "Arcade", "Classic", "Download", "Back"};
static const char *PUZZLE_GAMES[] = {"2048", "Tetris", "Back"};
static const char *const PUZZLE_SCORES[] = {"2048", "tetris", nullptr};
static const char *ARCADE_GAMES[] = {"Snake", "2D Race", "Back"};
static const char *const ARCADE_SCORES[] = {"snake", nullptr, nullptr};
static const char *CLASSIC_GAMES[] = {"Reaction test", "Flappy Birds", "Back"};
static const char *const CLASSIC_SCORES[] = {"reaction", "flappy", nullptr};

static void run_puzzle_menu(void) {
    while (true) {
        int idx = k85_run_list_menu("PUZZLE", PUZZLE_GAMES, 3, PUZZLE_SCORES);
        if (idx < 0) return;
        if (idx == 0) {
            k85_show_message("Coming soon\nA+B=back");
        } else if (idx == 1) {
            k85_run_tetris();
        }
    }
}

static void run_arcade_menu(void) {
    while (true) {
        int idx = k85_run_list_menu("ARCADE", ARCADE_GAMES, 3, ARCADE_SCORES);
        if (idx < 0) return;
        if (idx == 0) {
            k85_run_snake();
        } else {
            // TODO: 2D Race ещё не портирована — появится следующим слоем
            k85_show_message("Coming soon\nA+B=back");
        }
    }
}

static void run_classic_menu(void) {
    while (true) {
        int idx = k85_run_list_menu("CLASSIC", CLASSIC_GAMES, 3, CLASSIC_SCORES);
        if (idx < 0) return;
        // TODO: Reaction test/Flappy Birds ещё не портированы — появятся следующим слоем
        k85_show_message("Coming soon\nA+B=back");
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
void k85_run_store(void) {
    while (true) {
        int idx = k85_run_list_menu("STORE", STORE_CATEGORIES, 5, nullptr);
        if (idx < 0) return;
        if (idx == 0) run_puzzle_menu();
        else if (idx == 1) run_arcade_menu();
        else if (idx == 2) run_classic_menu();
        else if (idx == 3) run_download_menu();
    }
}
