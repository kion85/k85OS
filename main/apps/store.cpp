#include "store.h"
#include "list_menu.h"
#include "common.h"
#include "snake.h"
#include "tetris.h"

static const char *STORE_CATEGORIES[] = {"Puzzle", "Arcade", "Classic", "Back"};
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

void k85_run_store(void) {
    while (true) {
        int idx = k85_run_list_menu("STORE", STORE_CATEGORIES, 4, nullptr);
        if (idx < 0) return;
        if (idx == 0) run_puzzle_menu();
        else if (idx == 1) run_arcade_menu();
        else if (idx == 2) run_classic_menu();
    }
}
