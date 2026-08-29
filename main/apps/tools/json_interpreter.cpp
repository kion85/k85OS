#include "json_interpreter.h"
#include "common.h"
#include "list_menu.h"
#include "text_input.h"
#include "input.h"

#include "cJSON.h"
#include "esp_heap_caps.h"

#include "M5Unified.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define K85_JSON_GAMES_DIR "/littlefs/games_json"
#define K85_JSON_BUF_SIZE 16384
#define K85_JSON_MAX_FLAGS 16
#define K85_JSON_MAX_FILES 20
#define K85_JSON_MAX_CHOICES 8
#define K85_GRID_MAX_W 20
#define K85_GRID_MAX_H 14

static char s_flags[K85_JSON_MAX_FLAGS][24];
static int s_flag_count = 0;

static bool flag_is_set(const char *name) {
    for (int i = 0; i < s_flag_count; i++) {
        if (!strcmp(s_flags[i], name)) return true;
    }
    return false;
}

static void flag_set(const char *name) {
    if (flag_is_set(name)) return;
    if (s_flag_count >= K85_JSON_MAX_FLAGS) return;
    snprintf(s_flags[s_flag_count], sizeof(s_flags[0]), "%s", name);
    s_flag_count++;
}

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static int split_lines(char *text, const char *out[], int max_lines) {
    int count = 0;
    char *p = text;
    while (*p && count < max_lines) {
        out[count++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }
    if (count == 0) count = 1;
    return count;
}

// ---------- Текстовый квест (screens/choices/set/need) ----------
static void run_quest_game(cJSON *root) {
    cJSON *start = cJSON_GetObjectItem(root, "start");
    cJSON *screens = cJSON_GetObjectItem(root, "screens");
    if (!cJSON_IsString(start) || !cJSON_IsObject(screens)) {
        k85_show_message("Bad game format\n(need start+screens)\nA+B=back");
        wait_ab_exit();
        return;
    }

    s_flag_count = 0;
    char current[32];
    snprintf(current, sizeof(current), "%s", start->valuestring);

    while (true) {
        cJSON *screen = cJSON_GetObjectItem(screens, current);
        if (!screen) {
            k85_show_message("Screen not found\nA+B=back");
            break;
        }

        cJSON *set_flag = cJSON_GetObjectItem(screen, "set");
        if (cJSON_IsString(set_flag)) flag_set(set_flag->valuestring);

        cJSON *text_item = cJSON_GetObjectItem(screen, "text");
        char text_buf[256];
        snprintf(text_buf, sizeof(text_buf), "%s", cJSON_IsString(text_item) ? text_item->valuestring : "");

        const char *lines[10];
        int line_count = split_lines(text_buf, lines, 10);
        k85_area_show(lines, line_count, "GAME");

        cJSON *choices = cJSON_GetObjectItem(screen, "choices");
        if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
            break;
        }

        static char labels[K85_JSON_MAX_CHOICES][40];
        cJSON *valid_choices[K85_JSON_MAX_CHOICES];
        int n = 0;
        int total = cJSON_GetArraySize(choices);
        for (int i = 0; i < total && n < K85_JSON_MAX_CHOICES; i++) {
            cJSON *ch = cJSON_GetArrayItem(choices, i);
            cJSON *need = cJSON_GetObjectItem(ch, "need");
            if (cJSON_IsString(need) && !flag_is_set(need->valuestring)) continue;
            cJSON *label = cJSON_GetObjectItem(ch, "label");
            snprintf(labels[n], sizeof(labels[0]), "%s", cJSON_IsString(label) ? label->valuestring : "?");
            valid_choices[n] = ch;
            n++;
        }

        if (n == 0) {
            k85_show_message("No available choices\nA+B=back");
            break;
        }

        const char *items[K85_JSON_MAX_CHOICES];
        for (int i = 0; i < n; i++) items[i] = labels[i];

        int idx = k85_run_list_menu("CHOOSE", items, n, nullptr);
        if (idx < 0) break;

        cJSON *goto_item = cJSON_GetObjectItem(valid_choices[idx], "goto");
        if (!cJSON_IsString(goto_item)) break;
        snprintf(current, sizeof(current), "%s", goto_item->valuestring);
    }

    wait_ab_exit();
}

// ---------- Игра на сетке (grid/legend/controls: A=поворот, B=шаг) ----------
struct K85GridCell {
    uint32_t color;
    bool blocked;
    bool win;
    bool gameover;
    int score;
};

static uint32_t parse_hex_color(const char *s) {
    if (!s || s[0] != '#') return 0x000000;
    return (uint32_t)strtoul(s + 1, nullptr, 16);
}

static void run_grid_game(cJSON *root) {
    cJSON *width_j = cJSON_GetObjectItem(root, "width");
    cJSON *height_j = cJSON_GetObjectItem(root, "height");
    cJSON *rows_j = cJSON_GetObjectItem(root, "rows");
    cJSON *legend_j = cJSON_GetObjectItem(root, "legend");
    cJSON *title_j = cJSON_GetObjectItem(root, "title");

    if (!cJSON_IsNumber(width_j) || !cJSON_IsNumber(height_j) || !cJSON_IsArray(rows_j) || !cJSON_IsObject(legend_j)) {
        k85_show_message("Bad grid format\nA+B=back");
        wait_ab_exit();
        return;
    }

    int gw = width_j->valueint;
    int gh = height_j->valueint;
    if (gw < 1 || gw > K85_GRID_MAX_W || gh < 1 || gh > K85_GRID_MAX_H) {
        k85_show_message("Grid too large\n(max 20x14)\nA+B=back");
        wait_ab_exit();
        return;
    }

    static char grid[K85_GRID_MAX_H][K85_GRID_MAX_W + 1];
    int player_x = 0, player_y = 0;
    int facing = 0; // 0=up,1=right,2=down,3=left

    for (int y = 0; y < gh; y++) {
        cJSON *row = cJSON_GetArrayItem(rows_j, y);
        const char *rowstr = cJSON_IsString(row) ? row->valuestring : "";
        for (int x = 0; x < gw; x++) {
            char ch = (x < (int)strlen(rowstr)) ? rowstr[x] : '.';
            grid[y][x] = ch;
            char key[2] = { ch, 0 };
            cJSON *cell_def = cJSON_GetObjectItem(legend_j, key);
            if (cell_def) {
                cJSON *start = cJSON_GetObjectItem(cell_def, "start");
                if (cJSON_IsBool(start) && cJSON_IsTrue(start)) {
                    player_x = x;
                    player_y = y;
                }
            }
        }
        grid[y][gw] = 0;
    }

    int score = 0;
    bool game_over = false;
    bool won = false;

    int W = M5.Display.width();
    int H = M5.Display.height();
    int cell_px = W / gw;
    if ((H - 20) / gh < cell_px) cell_px = (H - 20) / gh;
    if (cell_px < 2) cell_px = 2;
    int origin_x = (W - cell_px * gw) / 2;
    int origin_y = 20 + ((H - 20) - cell_px * gh) / 2;

    char title[32];
    snprintf(title, sizeof(title), "%s", cJSON_IsString(title_j) ? title_j->valuestring : "Grid Game");

    auto get_cell_def = [&](char ch) -> cJSON * {
        char key[2] = { ch, 0 };
        return cJSON_GetObjectItem(legend_j, key);
    };

    auto draw = [&]() {
        M5.Display.fillScreen(0x000000);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xFFFFFF, 0x000000);
        M5.Display.setCursor(4, 2);
        M5.Display.printf("%s  Score:%d", title, score);

        for (int y = 0; y < gh; y++) {
            for (int x = 0; x < gw; x++) {
                cJSON *def = get_cell_def(grid[y][x]);
                uint32_t col = 0x000000;
                if (def) {
                    cJSON *color_j = cJSON_GetObjectItem(def, "color");
                    if (cJSON_IsString(color_j)) col = parse_hex_color(color_j->valuestring);
                }
                M5.Display.fillRect(origin_x + x * cell_px, origin_y + y * cell_px, cell_px - 1, cell_px - 1, col);
            }
        }

        // игрок — треугольник, направленный по facing
        int px = origin_x + player_x * cell_px + cell_px / 2;
        int py = origin_y + player_y * cell_px + cell_px / 2;
        int r = cell_px / 2 - 1;
        if (r < 2) r = 2;
        switch (facing) {
            case 0: M5.Display.fillTriangle(px, py - r, px - r, py + r, px + r, py + r, 0xFFFFFF); break;
            case 1: M5.Display.fillTriangle(px + r, py, px - r, py - r, px - r, py + r, 0xFFFFFF); break;
            case 2: M5.Display.fillTriangle(px, py + r, px - r, py - r, px + r, py - r, 0xFFFFFF); break;
            case 3: M5.Display.fillTriangle(px - r, py, px + r, py - r, px + r, py + r, 0xFFFFFF); break;
        }

        M5.Display.setTextColor(0xAAAAAA, 0x000000);
        M5.Display.setCursor(4, H - 10);
        M5.Display.print("A=turn B=step A+B=exit");
    };

    draw();
    while (!game_over && !won) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }

        if (k85_btn_a_pressed()) {
            facing = (facing + 1) % 4;
            draw();
        }
        if (k85_btn_b_pressed()) {
            int nx = player_x, ny = player_y;
            if (facing == 0) ny--;
            else if (facing == 1) nx++;
            else if (facing == 2) ny++;
            else if (facing == 3) nx--;

            if (nx >= 0 && nx < gw && ny >= 0 && ny < gh) {
                cJSON *def = get_cell_def(grid[ny][nx]);
                bool blocked = false;
                if (def) {
                    cJSON *b = cJSON_GetObjectItem(def, "blocked");
                    blocked = cJSON_IsBool(b) && cJSON_IsTrue(b);
                }
                if (!blocked) {
                    player_x = nx;
                    player_y = ny;
                    if (def) {
                        cJSON *sc = cJSON_GetObjectItem(def, "score");
                        if (cJSON_IsNumber(sc) && sc->valueint != 0) {
                            score += sc->valueint;
                            grid[ny][nx] = '.';
                        }
                        cJSON *win_j = cJSON_GetObjectItem(def, "win");
                        if (cJSON_IsBool(win_j) && cJSON_IsTrue(win_j)) won = true;
                        cJSON *go_j = cJSON_GetObjectItem(def, "gameover");
                        if (cJSON_IsBool(go_j) && cJSON_IsTrue(go_j)) game_over = true;
                    }
                }
            }
            draw();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    char msg[64];
    if (won) snprintf(msg, sizeof(msg), "You win!\nScore: %d\nA+B=back", score);
    else snprintf(msg, sizeof(msg), "Game over\nScore: %d\nA+B=back", score);
    k85_show_message(msg);
    wait_ab_exit();
}

// ---------- Общий загрузчик ----------
static void run_game_from_json(char *json_buf) {
    cJSON *root = cJSON_Parse(json_buf);
    if (!root) {
        k85_show_message("Invalid JSON\nA+B=back");
        wait_ab_exit();
        return;
    }

    cJSON *type_j = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type_j) && !strcmp(type_j->valuestring, "grid")) {
        run_grid_game(root);
    } else {
        run_quest_game(root);
    }

    cJSON_Delete(root);
}

void k85_run_json_interpreter(void) {
    struct stat st;
    if (stat(K85_JSON_GAMES_DIR, &st) != 0) {
        mkdir(K85_JSON_GAMES_DIR, 0755);
        k85_show_message("No games found\nUpload .json to\n/games_json via\nWiFi Hotspot\nA+B=back");
        wait_ab_exit();
        return;
    }

    static char names[K85_JSON_MAX_FILES][256];
    int count = 0;
    DIR *d = opendir(K85_JSON_GAMES_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != nullptr && count < K85_JSON_MAX_FILES) {
            size_t len = strlen(ent->d_name);
            if (len > 5 && !strcmp(ent->d_name + len - 5, ".json")) {
                snprintf(names[count], sizeof(names[0]), "%s", ent->d_name);
                count++;
            }
        }
        closedir(d);
    }

    if (count == 0) {
        k85_show_message("No .json games found\nUpload via WiFi\nHotspot -> games_json\nA+B=back");
        wait_ab_exit();
        return;
    }

    const char *items[K85_JSON_MAX_FILES + 1];
    for (int i = 0; i < count; i++) items[i] = names[i];
    items[count] = "Back";

    int idx = k85_run_list_menu("INTERPRETER", items, count + 1, nullptr);
    if (idx < 0 || idx >= count) return;

    char path[300];
    snprintf(path, sizeof(path), "%s/%s", K85_JSON_GAMES_DIR, names[idx]);

    char *buf = (char *)heap_caps_malloc(K85_JSON_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        k85_show_message("Out of memory\nA+B=back");
        wait_ab_exit();
        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        heap_caps_free(buf);
        k85_show_message("Cannot open file\nA+B=back");
        wait_ab_exit();
        return;
    }
    size_t r = fread(buf, 1, K85_JSON_BUF_SIZE - 1, f);
    fclose(f);
    buf[r] = 0;

    run_game_from_json(buf);
    heap_caps_free(buf);
}