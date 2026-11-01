#include "json_interpreter.h"
#include "config.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <cmath>
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
#define K85_JSON_MAX_BYTES (256 * 1024)
#define K85_JSON_MAX_FLAGS 48
#define K85_JSON_MAX_FILES 20
#define K85_JSON_MAX_CHOICES 12
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
        char text_buf[768];
        snprintf(text_buf, sizeof(text_buf), "%s", cJSON_IsString(text_item) ? text_item->valuestring : "");

        const char *lines[16];
        int line_count = split_lines(text_buf, lines, 16);
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
// K85SCR1: an example script game and the description of the format, written on the first start
static const char *SG_EXAMPLE_JSON = R"JSON({
  "type": "script",
  "name": "Dodger",
  "screen": [120, 160],
  "fps": 30,
  "background": "#050818",
  "vars": { "px": 56, "score": 0, "lives": 3, "inv": 0, "spawn_t": 0.5 },
  "sprites": {
    "ship": { "palette": { "#": "#00FFAA", "o": "#FFFFFF" },
              "rows": [ "...##...", "..####..", ".#oo##o.", "########", "#..##..#" ] }
  },
  "melodies": {
    "hit":  [[200, 80], [150, 120]],
    "lose": [[400, 120], [300, 120], [200, 300]]
  },
  "entities": {
    "block": {
      "w": 8, "h": 8,
      "on_tick": [ { "if": "self.y > 160", "do": [ { "remove": "self" }, { "set": { "score": "score + 1" } } ] } ],
      "draw": [ { "rect": ["self.x", "self.y", "self.w", "self.h"], "color": "#FF4455" } ]
    },
    "star": {
      "draw": [ { "pixel": ["self.x", "self.y"], "color": "#8899CC" } ]
    }
  },
  "on_start": [
    { "set": { "px": 56, "score": 0, "lives": 3, "inv": 0, "spawn_t": 0.5 } },
    { "repeat": 18, "do": [ { "spawn": "star", "set": { "x": "rand(120)", "y": "rand(160)", "vy": "10 + rand(30)" } } ] }
  ],
  "on_tick": [
    { "set": { "px": "clamp(px + (btn_b - btn_a) * 90 * dt, 0, 112)", "spawn_t": "spawn_t - dt", "inv": "max(0, inv - dt)" } },
    { "if": "spawn_t <= 0", "do": [
        { "spawn": "block", "set": { "x": "rand(112)", "y": -8, "vy": "50 + score * 2" } },
        { "set": { "spawn_t": "max(0.25, 0.9 - score * 0.02)" } } ] },
    { "if": "rand(100) < 12", "do": [ { "spawn": "star", "set": { "x": "rand(120)", "y": -2, "vy": "10 + rand(30)" } } ] },
    { "if": "inv <= 0 && hit(px, 148, 8, 6, block) > 0", "do": [ { "set": { "lives": "lives - 1", "inv": 1.2 } }, { "play": "hit" } ] },
    { "if": "lives <= 0", "do": [ { "play": "lose" }, { "end": "lose", "message": "Crashed" } ] }
  ],
  "draw": [
    { "if": "inv <= 0 || floor(time * 10) % 2 == 0", "do": [ { "sprite": "ship", "at": ["px", 148] } ] },
    { "text": ["Score {score}", 3, 3], "color": "#FFFFFF" },
    { "text": ["Lives {lives}", 117, 3], "color": "#FFAA55", "align": "right" }
  ]
}
)JSON";

static const char *SG_README_TXT =
    "k85OS script games  (\"type\": \"script\")\r\n"
    "Put a .json file in this folder and start it from Tools -> Interpreter.\r\n"
    "A+B always exits. After the game ends: B = play again. The best score is saved in save/<file>.hi.\r\n\r\n"
    "TOP LEVEL KEYS\r\n"
    " name, screen [w,h] (virtual size, scaled to fit; default = the whole display), fps 5..60,\r\n"
    " background \"#RRGGBB\", vars {name: number}, score_var (default \"score\"), hiscore true/false,\r\n"
    " start (scene name)\r\n"
    " sprites  {\"ship\": {\"palette\": {\"#\": \"#00FFAA\"}, \"rows\": [\"..##..\", \".####.\"]}}   '.' and ' ' are transparent\r\n"
    " melodies {\"win\": [[523,100],[659,100]]}      [frequency Hz, milliseconds]\r\n"
    " entities {\"block\": {...}}   objects: fields x y vx vy w h life a b c d e (defaults, numbers or expressions)\r\n"
    "     auto-move by vx,vy every tick (\"auto\": false stops it), \"on_tick\": [..], \"draw\": [..] (self.x ...),\r\n"
    "     life > 0 counts down in seconds and removes the object; objects 64 px outside the screen are removed.\r\n"
    " scenes   {\"play\": {on_start, on_tick, on_a, on_b, on_a_hold, on_b_hold, on_shake, back, draw}}\r\n"
    "     without \"scenes\" the same keys at the top level form one scene. back is drawn before the entities,\r\n"
    "     draw after them (HUD, player).\r\n\r\n"
    "STATEMENTS  (every list item is an object with one command)\r\n"
    " {\"set\": {\"x\": \"x + 1\", \"self.vy\": \"0\"}}            variables and fields of self\r\n"
    " {\"if\": \"expr\", \"do\": [..], \"else\": [..]}\r\n"
    " {\"repeat\": \"n\", \"do\": [..]}                          variable i = 0..n-1, max 256\r\n"
    " {\"each\": \"block\", \"do\": [..]}                         runs for every object of the kind, self.* inside\r\n"
    " {\"spawn\": \"block\", \"set\": {\"x\": \"rand(100)\", \"y\": 0, \"vy\": 50}}\r\n"
    " {\"remove\": \"self\"}   {\"remove_all\": \"block\"}\r\n"
    " {\"goto\": \"scene\"}    {\"end\": \"win|lose|quit\", \"message\": \"text {score}\"}\r\n"
    " {\"play\": [880, 100]}  or  {\"play\": \"melody\"}\r\n"
    "DRAWING (colors: \"#RRGGBB\" or an expression such as rgb(255, score, 0))\r\n"
    " {\"fill\": \"#000\"}  {\"gradient\": [\"#top\",\"#bottom\"]}  {\"rect\": [x,y,w,h], \"color\": .., \"fill\": true}\r\n"
    " {\"circle\": [x,y,r]}  {\"line\": [x1,y1,x2,y2]}  {\"pixel\": [x,y]}\r\n"
    " {\"text\": [\"Score {score}\", x, y], \"color\": .., \"size\": 1..4, \"align\": \"left|center|right\"}\r\n"
    " {\"sprite\": \"ship\", \"at\": [x,y], \"scale\": 1..8, \"flip\": false}\r\n\r\n"
    "EXPRESSIONS  numbers, variables, self.x, + - * / %, == != < > <= >=, && || !, ( )\r\n"
    " abs min max clamp(v,lo,hi) sin cos atan2 sqrt floor ceil round sign pow dist(x1,y1,x2,y2)\r\n"
    " rand(n) = whole number 0..n-1, randf() = 0..1, rgb(r,g,b)\r\n"
    " count(kind), hit(x,y,w,h,kind) = how many objects of the kind overlap the rectangle,\r\n"
    " overlap(x1,y1,w1,h1,x2,y2,w2,h2)\r\n"
    "BUILT-IN VARIABLES  dt (seconds per frame), time (seconds in the scene), frame, w, h, btn_a, btn_b (1 while held),\r\n"
    " tilt_x, tilt_y (accelerometer, in g), hi (best score)\r\n"
    "Unknown names become variables with the value 0. Spawn assignments see the NEW object as self.\r\n"
    "Text games: \"start\" + \"screens\" with \"text\" and \"choices\" (label, goto, need, set) still work as before,\r\n"
    "grid games (\"type\": \"grid\") too.\r\n";

static void sg_write_samples(void) {
    mkdir(K85_JSON_GAMES_DIR, 0755);
    struct stat sb;
    char p[96];
    snprintf(p, sizeof(p), "%s/dodger.json", K85_JSON_GAMES_DIR);
    if (stat(p, &sb) != 0) { FILE *f = fopen(p, "w"); if (f) { fputs(SG_EXAMPLE_JSON, f); fclose(f); } }
    snprintf(p, sizeof(p), "%s/README_script_games.txt", K85_JSON_GAMES_DIR);
    if (stat(p, &sb) != 0) { FILE *f = fopen(p, "w"); if (f) { fputs(SG_README_TXT, f); fclose(f); } }
}
// ===================== K85SCR1: scripted games (graphics and logic from JSON) =====================
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define SG_MAXN       8192
#define SG_MAXARGS    3072
#define SG_MAXS       2048
#define SG_MAXL       4096
#define SG_MAXLISTS   1024
#define SG_MAXASG     2048
#define SG_MAXTMPL    512
#define SG_MAXPART    1536
#define SG_POOL       32768
#define SG_MAXVARS    256
#define SG_MAXENT     160
#define SG_MAXKINDS   16
#define SG_MAXSCENES  16
#define SG_MAXSPR     32
#define SG_MAXMEL     16
#define SG_MAXNOTES   64
#define SG_OPS_MAX    80000
#define SG_MAX_DEPTH  24

enum { SGN_NUM, SGN_VAR, SGN_SELF, SGN_NEG, SGN_NOT, SGN_ADD, SGN_SUB, SGN_MUL, SGN_DIV, SGN_MOD,
       SGN_EQ, SGN_NE, SGN_LT, SGN_GT, SGN_LE, SGN_GE, SGN_AND, SGN_OR, SGN_CALL };
enum { SGF_ABS, SGF_MIN, SGF_MAX, SGF_CLAMP, SGF_SIN, SGF_COS, SGF_ATAN2, SGF_SQRT, SGF_FLOOR, SGF_CEIL,
       SGF_ROUND, SGF_RAND, SGF_RANDF, SGF_COUNT, SGF_OVERLAP, SGF_RGB, SGF_SIGN, SGF_POW, SGF_DIST, SGF_HIT };
enum { SGFLD_X, SGFLD_Y, SGFLD_VX, SGFLD_VY, SGFLD_W, SGFLD_H, SGFLD_LIFE, SGFLD_A, SGFLD_B, SGFLD_C, SGFLD_D, SGFLD_E, SGFLD_COUNT };
enum { SGV_SET, SGV_IF, SGV_REPEAT, SGV_EACH, SGV_SPAWN, SGV_REMOVE, SGV_REMOVE_ALL, SGV_GOTO, SGV_END, SGV_PLAY,
       SGV_FILL, SGV_GRADIENT, SGV_RECT, SGV_CIRCLE, SGV_LINE, SGV_PIXEL, SGV_TEXT, SGV_SPRITE };
enum { SGB_DT, SGB_TIME, SGB_FRAME, SGB_W, SGB_H, SGB_BTN_A, SGB_BTN_B, SGB_TILT_X, SGB_TILT_Y, SGB_HI, SGB_COUNT };

static const char *const SG_FLD_NAMES[SGFLD_COUNT] = { "x", "y", "vx", "vy", "w", "h", "life", "a", "b", "c", "d", "e" };
static const char *const SG_BUILTIN_NAMES[SGB_COUNT] = { "dt", "time", "frame", "w", "h", "btn_a", "btn_b", "tilt_x", "tilt_y", "hi" };

struct SgNode { uint8_t op, fn; int16_t argc; int32_t a, b; double v; };
struct SgAsg { int32_t target; int32_t expr; };          // target >= 0: global variable, < 0: -(1 + field) of the entity
struct SgPart { int32_t lit; int32_t expr; };
struct SgTmpl { int32_t first, n; };
struct SgList { int32_t first, n; };
struct SgStmt {
    uint8_t verb, flags, size, align;
    int16_t kind;
    int32_t ex[5];
    int32_t cond, blk_a, blk_b, asg_first, asg_n, tmpl;
    uint32_t col, col2;
    int32_t colx, colx2;
};
struct SgKind { char name[20]; int32_t def[SGFLD_COUNT]; int32_t on_tick, draw; bool auto_move; };
struct SgScene { char name[20]; int32_t on_start, on_tick, on_a, on_b, on_a_hold, on_b_hold, on_shake, draw, back; };
struct SgSprite { char name[20]; int w, h; int32_t *px; };
struct SgMel { char name[20]; int n; uint16_t hz[SG_MAXNOTES]; uint16_t ms[SG_MAXNOTES]; };
struct SgEnt { double f[SGFLD_COUNT]; int8_t kind; bool alive; };

struct SgGame {
    SgNode *nodes; int nn;
    int32_t *args; int nargs;
    SgStmt *st; int ns;
    int32_t *items; int ni;
    SgList *lists; int nl;
    SgAsg *asg; int nasg;
    SgTmpl *tmpl; int ntmpl;
    SgPart *parts; int nparts;
    char *pool; int npool;
    char names[SG_MAXVARS][20];
    double vars[SG_MAXVARS];
    double init[SG_MAXVARS];
    int nvars, var_i;
    SgKind kinds[SG_MAXKINDS]; int nkinds;
    SgScene scenes[SG_MAXSCENES]; int nscenes;
    SgSprite spr[SG_MAXSPR]; int nspr;
    SgMel mel[SG_MAXMEL]; int nmel;
    SgEnt ents[SG_MAXENT]; int nent_high;
    char title[32];
    int vw, vh, fps;
    uint32_t bg;
    int score_var, hi;
    bool hiscore_on;
    int start_scene, cur_scene, pending_scene;
    int ended;
    char end_msg[40];
    long ops;
    int depth;
    bool failed;
    char err[160];
    M5Canvas *cv;
    int mel_idx, mel_pos;
    uint32_t mel_next;
};
static SgGame *sgg = nullptr;

static void sg_err(const char *what, const char *detail = nullptr) {
    if (sgg->failed) return;
    sgg->failed = true;
    if (detail) snprintf(sgg->err, sizeof(sgg->err), "%s: %s", what, detail);
    else snprintf(sgg->err, sizeof(sgg->err), "%s", what);
}
static void *sg_alloc(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static int sg_pool_add(const char *s, size_t n) {
    if (sgg->npool + (int)n + 1 > SG_POOL) { sg_err("too much text in the game"); return -1; }
    int off = sgg->npool;
    memcpy(sgg->pool + off, s, n);
    sgg->pool[off + (int)n] = 0;
    sgg->npool += (int)n + 1;
    return off;
}

// ---------- names ----------
static int sg_var_find(const char *name) { for (int i = 0; i < sgg->nvars; i++) if (!strcmp(sgg->names[i], name)) return i; return -1; }
static int sg_var_get(const char *name, bool create) {
    int i = sg_var_find(name);
    if (i >= 0 || !create) return i;
    if (sgg->nvars >= SG_MAXVARS) { sg_err("too many variables"); return -1; }
    snprintf(sgg->names[sgg->nvars], sizeof(sgg->names[0]), "%.19s", name);
    sgg->vars[sgg->nvars] = 0.0;
    return sgg->nvars++;
}
static int sg_kind_find(const char *n) { for (int i = 0; i < sgg->nkinds; i++) if (!strcmp(sgg->kinds[i].name, n)) return i; return -1; }
static int sg_scene_find(const char *n) { for (int i = 0; i < sgg->nscenes; i++) if (!strcmp(sgg->scenes[i].name, n)) return i; return -1; }
static int sg_spr_find(const char *n) { for (int i = 0; i < sgg->nspr; i++) if (!strcmp(sgg->spr[i].name, n)) return i; return -1; }
static int sg_mel_find(const char *n) { for (int i = 0; i < sgg->nmel; i++) if (!strcmp(sgg->mel[i].name, n)) return i; return -1; }

// ---------- expressions: parsed once into a small tree ----------
static int sg_mk(uint8_t op, int32_t a, int32_t b, double v = 0.0) {
    if (sgg->nn >= SG_MAXN) { sg_err("game is too large (expressions)"); return -1; }
    SgNode &n = sgg->nodes[sgg->nn];
    n.op = op; n.fn = 0; n.argc = 0; n.a = a; n.b = b; n.v = v;
    return sgg->nn++;
}

struct SgLex { const char *s; int p; };
static void sg_ws(SgLex &L) { while (L.s[L.p] == ' ' || L.s[L.p] == '\t' || L.s[L.p] == '\r' || L.s[L.p] == '\n') L.p++; }
static bool sg_is_id0(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool sg_is_id(char c) { return sg_is_id0(c) || (c >= '0' && c <= '9') || c == '.'; }

static int sg_or(SgLex &L);
static int sg_call(SgLex &L, const char *name);

static int sg_ident(const char *id) {
    if (!strncmp(id, "self.", 5)) {
        for (int f = 0; f < SGFLD_COUNT; f++) if (!strcmp(id + 5, SG_FLD_NAMES[f])) return sg_mk(SGN_SELF, f, 0);
        sg_err("unknown entity field", id);
        return -1;
    }
    if (!strcmp(id, "true")) return sg_mk(SGN_NUM, 0, 0, 1.0);
    if (!strcmp(id, "false")) return sg_mk(SGN_NUM, 0, 0, 0.0);
    if (strchr(id, '.')) { sg_err("bad name", id); return -1; }
    int v = sg_var_get(id, true);
    if (v < 0) return -1;
    return sg_mk(SGN_VAR, v, 0);
}

static int sg_kind_arg(SgLex &L) {
    sg_ws(L);
    char nm[24];
    int n = 0;
    char q = L.s[L.p];
    if (q == '"' || q == '\'') {
        L.p++;
        while (L.s[L.p] && L.s[L.p] != q && n < 23) nm[n++] = L.s[L.p++];
        if (L.s[L.p] == q) L.p++;
    } else {
        while (sg_is_id(L.s[L.p]) && n < 23) nm[n++] = L.s[L.p++];
    }
    nm[n] = 0;
    int k = sg_kind_find(nm);
    if (k < 0) { sg_err("unknown entity kind", nm); return -1; }
    return k;
}

struct SgFnDef { const char *name; uint8_t id, minc, maxc; };
static const SgFnDef SG_FN[] = {
    {"abs", SGF_ABS, 1, 1}, {"min", SGF_MIN, 2, 2}, {"max", SGF_MAX, 2, 2}, {"clamp", SGF_CLAMP, 3, 3},
    {"sin", SGF_SIN, 1, 1}, {"cos", SGF_COS, 1, 1}, {"atan2", SGF_ATAN2, 2, 2}, {"sqrt", SGF_SQRT, 1, 1},
    {"floor", SGF_FLOOR, 1, 1}, {"ceil", SGF_CEIL, 1, 1}, {"round", SGF_ROUND, 1, 1}, {"rand", SGF_RAND, 1, 1},
    {"randf", SGF_RANDF, 0, 0}, {"count", SGF_COUNT, 0, 0}, {"overlap", SGF_OVERLAP, 8, 8}, {"rgb", SGF_RGB, 3, 3},
    {"sign", SGF_SIGN, 1, 1}, {"pow", SGF_POW, 2, 2}, {"dist", SGF_DIST, 4, 4}, {"hit", SGF_HIT, 4, 4},
};

static int sg_call(SgLex &L, const char *name) {
    const SgFnDef *fd = nullptr;
    for (size_t i = 0; i < sizeof(SG_FN) / sizeof(SG_FN[0]); i++) if (!strcmp(SG_FN[i].name, name)) { fd = &SG_FN[i]; break; }
    if (!fd) { sg_err("unknown function", name); return -1; }
    L.p++;   // the (
    int a[8];
    int argc = 0;
    int kind = -1;
    sg_ws(L);
    if (fd->id == SGF_COUNT) {
        kind = sg_kind_arg(L);
        if (kind < 0) return -1;
    } else if (L.s[L.p] != ')') {
        while (true) {
            if (fd->id == SGF_HIT && argc == 4) { kind = sg_kind_arg(L); if (kind < 0) return -1; break; }
            if (argc >= 8) { sg_err("too many arguments", name); return -1; }
            int e = sg_or(L);
            if (e < 0) return -1;
            a[argc++] = e;
            sg_ws(L);
            if (L.s[L.p] == ',') { L.p++; continue; }
            break;
        }
    }
    sg_ws(L);
    if (L.s[L.p] != ')') { sg_err("missing ) after the arguments of", name); return -1; }
    L.p++;
    if (fd->id == SGF_HIT) {
        if (argc != 4 || kind < 0) { sg_err("hit(x, y, w, h, kind) needs 5 arguments"); return -1; }
    } else if (fd->id != SGF_COUNT && (argc < fd->minc || argc > fd->maxc)) {
        sg_err("wrong number of arguments for", name);
        return -1;
    }
    if (sgg->nargs + argc > SG_MAXARGS) { sg_err("game is too large (arguments)"); return -1; }
    int first = sgg->nargs;
    for (int i = 0; i < argc; i++) sgg->args[sgg->nargs++] = a[i];
    int n = sg_mk(SGN_CALL, first, kind);
    if (n < 0) return -1;
    sgg->nodes[n].fn = fd->id;
    sgg->nodes[n].argc = (int16_t)argc;
    return n;
}

static int sg_primary(SgLex &L) {
    sg_ws(L);
    char c = L.s[L.p];
    if (c == '(') {
        L.p++;
        int e = sg_or(L);
        if (e < 0) return -1;
        sg_ws(L);
        if (L.s[L.p] != ')') { sg_err("missing ) in", L.s); return -1; }
        L.p++;
        return e;
    }
    if ((c >= '0' && c <= '9') || (c == '.' && L.s[L.p + 1] >= '0' && L.s[L.p + 1] <= '9')) {
        char *end = nullptr;
        double v = strtod(L.s + L.p, &end);
        L.p = (int)(end - L.s);
        return sg_mk(SGN_NUM, 0, 0, v);
    }
    if (sg_is_id0(c)) {
        char id[40];
        int n = 0;
        while (sg_is_id(L.s[L.p]) && n < 39) id[n++] = L.s[L.p++];
        id[n] = 0;
        sg_ws(L);
        if (L.s[L.p] == '(') return sg_call(L, id);
        return sg_ident(id);
    }
    sg_err("unexpected character in", L.s);
    return -1;
}
static int sg_unary(SgLex &L) {
    sg_ws(L);
    if (L.s[L.p] == '-') { L.p++; int e = sg_unary(L); return e < 0 ? -1 : sg_mk(SGN_NEG, e, 0); }
    if (L.s[L.p] == '!' && L.s[L.p + 1] != '=') { L.p++; int e = sg_unary(L); return e < 0 ? -1 : sg_mk(SGN_NOT, e, 0); }
    if (L.s[L.p] == '+') { L.p++; return sg_unary(L); }
    return sg_primary(L);
}
static int sg_mul(SgLex &L) {
    int l = sg_unary(L);
    while (l >= 0) {
        sg_ws(L);
        char c = L.s[L.p];
        if (c != '*' && c != '/' && c != '%') break;
        L.p++;
        int r = sg_unary(L);
        if (r < 0) return -1;
        l = sg_mk(c == '*' ? SGN_MUL : (c == '/' ? SGN_DIV : SGN_MOD), l, r);
        if (l < 0) return -1;
    }
    return l;
}
static int sg_add(SgLex &L) {
    int l = sg_mul(L);
    while (l >= 0) {
        sg_ws(L);
        char c = L.s[L.p];
        if (c != '+' && c != '-') break;
        L.p++;
        int r = sg_mul(L);
        if (r < 0) return -1;
        l = sg_mk(c == '+' ? SGN_ADD : SGN_SUB, l, r);
        if (l < 0) return -1;
    }
    return l;
}
static int sg_rel(SgLex &L) {
    int l = sg_add(L);
    while (l >= 0) {
        sg_ws(L);
        char c = L.s[L.p], d = L.s[L.p + 1];
        int op = -1, len = 1;
        if (c == '<' && d == '=') { op = SGN_LE; len = 2; }
        else if (c == '>' && d == '=') { op = SGN_GE; len = 2; }
        else if (c == '<') op = SGN_LT;
        else if (c == '>') op = SGN_GT;
        if (op < 0) break;
        L.p += len;
        int r = sg_add(L);
        if (r < 0) return -1;
        l = sg_mk((uint8_t)op, l, r);
        if (l < 0) return -1;
    }
    return l;
}
static int sg_eq(SgLex &L) {
    int l = sg_rel(L);
    while (l >= 0) {
        sg_ws(L);
        char c = L.s[L.p], d = L.s[L.p + 1];
        int op = -1;
        if (c == '=' && d == '=') op = SGN_EQ;
        else if (c == '!' && d == '=') op = SGN_NE;
        if (op < 0) break;
        L.p += 2;
        int r = sg_rel(L);
        if (r < 0) return -1;
        l = sg_mk((uint8_t)op, l, r);
        if (l < 0) return -1;
    }
    return l;
}
static int sg_and(SgLex &L) {
    int l = sg_eq(L);
    while (l >= 0) {
        sg_ws(L);
        if (!(L.s[L.p] == '&' && L.s[L.p + 1] == '&')) break;
        L.p += 2;
        int r = sg_eq(L);
        if (r < 0) return -1;
        l = sg_mk(SGN_AND, l, r);
        if (l < 0) return -1;
    }
    return l;
}
static int sg_or(SgLex &L) {
    int l = sg_and(L);
    while (l >= 0) {
        sg_ws(L);
        if (!(L.s[L.p] == '|' && L.s[L.p + 1] == '|')) break;
        L.p += 2;
        int r = sg_and(L);
        if (r < 0) return -1;
        l = sg_mk(SGN_OR, l, r);
        if (l < 0) return -1;
    }
    return l;
}
static int sg_expr_str(const char *s) {
    if (strlen(s) > 160) { sg_err("expression is too long (max 160 characters)", s); return -1; }
    SgLex L = { s, 0 };
    int e = sg_or(L);
    if (e < 0) return -1;
    sg_ws(L);
    if (L.s[L.p]) { sg_err("unexpected text in", s); return -1; }
    return e;
}
static int sg_expr_item(const cJSON *it) {
    if (cJSON_IsNumber(it)) return sg_mk(SGN_NUM, 0, 0, it->valuedouble);
    if (cJSON_IsBool(it)) return sg_mk(SGN_NUM, 0, 0, cJSON_IsTrue(it) ? 1.0 : 0.0);
    if (cJSON_IsString(it) && it->valuestring) return sg_expr_str(it->valuestring);
    sg_err("expected a number or an expression");
    return -1;
}

// ---------- texts with {expression} parts ----------
static int sg_tmpl_compile(const char *s) {
    if (sgg->ntmpl >= SG_MAXTMPL) { sg_err("too many texts"); return -1; }
    int ti = sgg->ntmpl++;
    sgg->tmpl[ti].first = sgg->nparts;
    sgg->tmpl[ti].n = 0;
    char lit[96];
    int ln = 0;
    const char *p = s;
    while (true) {
        bool at_end = (*p == 0);
        bool brace = (*p == '{');
        if ((at_end || brace || ln >= 90) && ln > 0) {
            if (sgg->nparts >= SG_MAXPART) { sg_err("too many texts"); return -1; }
            lit[ln] = 0;
            int off = sg_pool_add(lit, (size_t)ln);
            if (off < 0) return -1;
            sgg->parts[sgg->nparts].lit = off;
            sgg->parts[sgg->nparts].expr = -1;
            sgg->nparts++;
            sgg->tmpl[ti].n++;
            ln = 0;
        }
        if (at_end) break;
        if (brace) {
            const char *e = strchr(p, '}');
            if (!e) { sg_err("missing } in the text", s); return -1; }
            char ex[96];
            size_t n = (size_t)(e - p - 1);
            if (n >= sizeof(ex)) n = sizeof(ex) - 1;
            memcpy(ex, p + 1, n);
            ex[n] = 0;
            int en = sg_expr_str(ex);
            if (en < 0) return -1;
            if (sgg->nparts >= SG_MAXPART) { sg_err("too many texts"); return -1; }
            sgg->parts[sgg->nparts].lit = -1;
            sgg->parts[sgg->nparts].expr = en;
            sgg->nparts++;
            sgg->tmpl[ti].n++;
            p = e + 1;
            continue;
        }
        if (ln < 90) lit[ln++] = *p;
        p++;
    }
    return ti;
}

// ---------- colors ----------
static bool sg_color_const(const char *s, uint32_t *out) {
    if (s[0] != '#') return false;
    *out = (uint32_t)strtoul(s + 1, nullptr, 16) & 0xFFFFFF;
    return true;
}
static bool sg_color_item(const cJSON *c, uint32_t *col, int32_t *colx) {
    if (!c) return true;
    if (cJSON_IsNumber(c)) { *col = (uint32_t)c->valuedouble & 0xFFFFFF; return true; }
    if (cJSON_IsString(c) && c->valuestring) {
        if (sg_color_const(c->valuestring, col)) return true;
        int e = sg_expr_str(c->valuestring);
        if (e < 0) return false;
        *colx = e;
        return true;
    }
    sg_err("bad color");
    return false;
}

// ---------- evaluation ----------
static double sg_ev(int n, const SgEnt *self);
static bool sg_overlap(double x1, double y1, double w1, double h1, double x2, double y2, double w2, double h2) {
    return x1 < x2 + w2 && x2 < x1 + w1 && y1 < y2 + h2 && y2 < y1 + h1;
}
static double sg_ev_call(const SgNode &n, const SgEnt *self) {
    double a[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < n.argc && i < 8; i++) a[i] = sg_ev(sgg->args[n.a + i], self);
    switch (n.fn) {
        case SGF_ABS: return fabs(a[0]);
        case SGF_MIN: return a[0] < a[1] ? a[0] : a[1];
        case SGF_MAX: return a[0] > a[1] ? a[0] : a[1];
        case SGF_CLAMP: return a[0] < a[1] ? a[1] : (a[0] > a[2] ? a[2] : a[0]);
        case SGF_SIN: return sin(a[0]);
        case SGF_COS: return cos(a[0]);
        case SGF_ATAN2: return atan2(a[0], a[1]);
        case SGF_SQRT: return a[0] <= 0.0 ? 0.0 : sqrt(a[0]);
        case SGF_FLOOR: return floor(a[0]);
        case SGF_CEIL: return ceil(a[0]);
        case SGF_ROUND: return floor(a[0] + 0.5);
        case SGF_RAND: { uint32_t m = a[0] < 1.0 ? 1u : (uint32_t)a[0]; return (double)(esp_random() % m); }
        case SGF_RANDF: return (double)(esp_random() & 0xFFFFFFu) / 16777216.0;
        case SGF_COUNT: {
            int c = 0;
            for (int i = 0; i < sgg->nent_high; i++) if (sgg->ents[i].alive && sgg->ents[i].kind == n.b) c++;
            return (double)c;
        }
        case SGF_OVERLAP: return sg_overlap(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]) ? 1.0 : 0.0;
        case SGF_RGB: {
            int r = (int)a[0], g2 = (int)a[1], b = (int)a[2];
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g2 = g2 < 0 ? 0 : (g2 > 255 ? 255 : g2);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            return (double)(((uint32_t)r << 16) | ((uint32_t)g2 << 8) | (uint32_t)b);
        }
        case SGF_SIGN: return a[0] > 0.0 ? 1.0 : (a[0] < 0.0 ? -1.0 : 0.0);
        case SGF_POW: return pow(a[0], a[1]);
        case SGF_DIST: { double dx = a[2] - a[0], dy = a[3] - a[1]; return sqrt(dx * dx + dy * dy); }
        case SGF_HIT: {
            int c = 0;
            for (int i = 0; i < sgg->nent_high; i++) {
                const SgEnt &e = sgg->ents[i];
                if (e.alive && e.kind == n.b && sg_overlap(a[0], a[1], a[2], a[3], e.f[SGFLD_X], e.f[SGFLD_Y], e.f[SGFLD_W], e.f[SGFLD_H])) c++;
            }
            return (double)c;
        }
    }
    return 0.0;
}
static double sg_ev(int n, const SgEnt *self) {
    if (n < 0) return 0.0;
    const SgNode &nd = sgg->nodes[n];
    switch (nd.op) {
        case SGN_NUM: return nd.v;
        case SGN_VAR: return sgg->vars[nd.a];
        case SGN_SELF: return self ? self->f[nd.a] : 0.0;
        case SGN_NEG: return -sg_ev(nd.a, self);
        case SGN_NOT: return sg_ev(nd.a, self) == 0.0 ? 1.0 : 0.0;
        case SGN_ADD: return sg_ev(nd.a, self) + sg_ev(nd.b, self);
        case SGN_SUB: return sg_ev(nd.a, self) - sg_ev(nd.b, self);
        case SGN_MUL: return sg_ev(nd.a, self) * sg_ev(nd.b, self);
        case SGN_DIV: { double d = sg_ev(nd.b, self); return d == 0.0 ? 0.0 : sg_ev(nd.a, self) / d; }
        case SGN_MOD: { double d = sg_ev(nd.b, self); return d == 0.0 ? 0.0 : fmod(sg_ev(nd.a, self), d); }
        case SGN_EQ: return fabs(sg_ev(nd.a, self) - sg_ev(nd.b, self)) < 1e-9 ? 1.0 : 0.0;
        case SGN_NE: return fabs(sg_ev(nd.a, self) - sg_ev(nd.b, self)) < 1e-9 ? 0.0 : 1.0;
        case SGN_LT: return sg_ev(nd.a, self) < sg_ev(nd.b, self) ? 1.0 : 0.0;
        case SGN_GT: return sg_ev(nd.a, self) > sg_ev(nd.b, self) ? 1.0 : 0.0;
        case SGN_LE: return sg_ev(nd.a, self) <= sg_ev(nd.b, self) ? 1.0 : 0.0;
        case SGN_GE: return sg_ev(nd.a, self) >= sg_ev(nd.b, self) ? 1.0 : 0.0;
        case SGN_AND: return (sg_ev(nd.a, self) != 0.0 && sg_ev(nd.b, self) != 0.0) ? 1.0 : 0.0;
        case SGN_OR: return (sg_ev(nd.a, self) != 0.0 || sg_ev(nd.b, self) != 0.0) ? 1.0 : 0.0;
        case SGN_CALL: return sg_ev_call(nd, self);
    }
    return 0.0;
}
static int sg_iv(int n, const SgEnt *self) { return (int)floor(sg_ev(n, self)); }
static uint32_t sg_col(uint32_t c, int32_t cx, const SgEnt *self) { return cx >= 0 ? (((uint32_t)sg_ev(cx, self)) & 0xFFFFFF) : c; }

static void sg_tmpl_eval(int ti, const SgEnt *self, char *out, size_t cap) {
    out[0] = 0;
    if (ti < 0) return;
    size_t len = 0;
    const SgTmpl &t = sgg->tmpl[ti];
    for (int i = 0; i < t.n && len + 1 < cap; i++) {
        const SgPart &pt = sgg->parts[t.first + i];
        char tmp[40];
        const char *src = tmp;
        if (pt.lit >= 0) {
            src = sgg->pool + pt.lit;
        } else {
            double v = sg_ev(pt.expr, self);
            if (fabs(v - floor(v)) < 1e-9 && fabs(v) < 1e9) snprintf(tmp, sizeof(tmp), "%ld", (long)v);
            else snprintf(tmp, sizeof(tmp), "%.2f", v);
        }
        size_t l = strlen(src);
        if (l > cap - 1 - len) l = cap - 1 - len;
        memcpy(out + len, src, l);
        len += l;
        out[len] = 0;
    }
}

// ---------- compiling statements ----------
static int sg_list(const cJSON *arr);
static int sg_depth_c = 0;

static bool sg_exprs(const cJSON *arr, int32_t *out, int n, const char *what) {
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < n) {
        char m[48];
        snprintf(m, sizeof(m), "%s needs a list of %d values", what, n);
        sg_err(m);
        return false;
    }
    for (int i = 0; i < n; i++) {
        out[i] = sg_expr_item(cJSON_GetArrayItem(arr, i));
        if (out[i] < 0) return false;
    }
    return true;
}

static bool sg_assigns(const cJSON *obj, SgStmt *s, bool fields_first) {
    s->asg_first = sgg->nasg;
    s->asg_n = 0;
    for (const cJSON *c = obj->child; c; c = c->next) {
        if (!c->string) continue;
        if (sgg->nasg >= SG_MAXASG) { sg_err("too many assignments"); return false; }
        int target = 0;
        int f = -1;
        const char *nm = c->string;
        if (!strncmp(nm, "self.", 5)) nm += 5, f = -2;
        for (int k = 0; k < SGFLD_COUNT && f != -3; k++) {
            if (!strcmp(nm, SG_FLD_NAMES[k]) && (fields_first || f == -2)) { f = k; break; }
        }
        if (f == -2) { sg_err("unknown entity field", c->string); return false; }
        if (f >= 0) target = -(1 + f);
        else { target = sg_var_get(c->string, true); if (target < 0) return false; }
        int e = sg_expr_item(c);
        if (e < 0) return false;
        sgg->asg[sgg->nasg].target = target;
        sgg->asg[sgg->nasg].expr = e;
        sgg->nasg++;
        s->asg_n++;
    }
    return true;
}

#define SG_GET(o, k) cJSON_GetObjectItemCaseSensitive((o), (k))

static int sg_stmt(const cJSON *o) {
    if (!cJSON_IsObject(o)) { sg_err("a statement must be an object { ... }"); return -1; }
    if (sgg->ns >= SG_MAXS) { sg_err("game is too large (statements)"); return -1; }
    int si = sgg->ns++;
    SgStmt &s = sgg->st[si];
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < 5; i++) s.ex[i] = -1;
    s.cond = s.blk_a = s.blk_b = s.tmpl = s.colx = s.colx2 = -1;
    s.kind = -1;
    s.size = 1;
    s.col = 0xFFFFFF;
    s.flags = 1;     // bit0 = filled shape, bit1 = mirrored sprite
    const cJSON *v;

    if ((v = SG_GET(o, "spawn")) != nullptr) {
        s.verb = SGV_SPAWN;
        if (!cJSON_IsString(v)) { sg_err("\"spawn\" needs an entity name"); return -1; }
        s.kind = (int16_t)sg_kind_find(v->valuestring);
        if (s.kind < 0) { sg_err("unknown entity kind", v->valuestring); return -1; }
        const cJSON *st = SG_GET(o, "set");
        if (cJSON_IsObject(st) && !sg_assigns(st, &s, true)) return -1;
        return si;
    }
    if ((v = SG_GET(o, "each")) != nullptr) {
        s.verb = SGV_EACH;
        if (!cJSON_IsString(v)) { sg_err("\"each\" needs an entity name"); return -1; }
        s.kind = (int16_t)sg_kind_find(v->valuestring);
        if (s.kind < 0) { sg_err("unknown entity kind", v->valuestring); return -1; }
        s.blk_a = sg_list(SG_GET(o, "do"));
        return sgg->failed ? -1 : si;
    }
    if ((v = SG_GET(o, "repeat")) != nullptr) {
        s.verb = SGV_REPEAT;
        s.cond = sg_expr_item(v);
        if (s.cond < 0) return -1;
        s.blk_a = sg_list(SG_GET(o, "do"));
        return sgg->failed ? -1 : si;
    }
    if ((v = SG_GET(o, "if")) != nullptr) {
        s.verb = SGV_IF;
        s.cond = sg_expr_item(v);
        if (s.cond < 0) return -1;
        s.blk_a = sg_list(SG_GET(o, "do"));
        if (sgg->failed) return -1;
        s.blk_b = sg_list(SG_GET(o, "else"));
        return sgg->failed ? -1 : si;
    }
    if ((v = SG_GET(o, "goto")) != nullptr) {
        s.verb = SGV_GOTO;
        if (!cJSON_IsString(v)) { sg_err("\"goto\" needs a scene name"); return -1; }
        s.kind = (int16_t)sg_scene_find(v->valuestring);
        if (s.kind < 0) { sg_err("unknown scene", v->valuestring); return -1; }
        return si;
    }
    if ((v = SG_GET(o, "end")) != nullptr) {
        s.verb = SGV_END;
        const char *w = cJSON_IsString(v) ? v->valuestring : "quit";
        s.kind = !strcmp(w, "win") ? 0 : (!strcmp(w, "lose") ? 1 : 2);
        const cJSON *m = SG_GET(o, "message");
        if (cJSON_IsString(m) && m->valuestring) { s.tmpl = sg_tmpl_compile(m->valuestring); if (s.tmpl < 0) return -1; }
        return si;
    }
    if ((v = SG_GET(o, "play")) != nullptr) {
        s.verb = SGV_PLAY;
        if (cJSON_IsString(v)) {
            s.kind = (int16_t)sg_mel_find(v->valuestring);
            if (s.kind < 0) { sg_err("unknown melody", v->valuestring); return -1; }
        } else if (!sg_exprs(v, s.ex, 2, "\"play\"")) {
            return -1;
        }
        return si;
    }
    if ((v = SG_GET(o, "remove_all")) != nullptr) {
        s.verb = SGV_REMOVE_ALL;
        if (!cJSON_IsString(v)) { sg_err("\"remove_all\" needs an entity name"); return -1; }
        s.kind = (int16_t)sg_kind_find(v->valuestring);
        if (s.kind < 0) { sg_err("unknown entity kind", v->valuestring); return -1; }
        return si;
    }
    if ((v = SG_GET(o, "remove")) != nullptr) {
        s.verb = SGV_REMOVE;
        return si;
    }
    if ((v = SG_GET(o, "gradient")) != nullptr) {
        s.verb = SGV_GRADIENT;
        const cJSON *c0 = cJSON_IsArray(v) ? cJSON_GetArrayItem(v, 0) : nullptr;
        const cJSON *c1 = cJSON_IsArray(v) ? cJSON_GetArrayItem(v, 1) : nullptr;
        if (!cJSON_IsString(c0) || !cJSON_IsString(c1) || !sg_color_const(c0->valuestring, &s.col) || !sg_color_const(c1->valuestring, &s.col2)) {
            sg_err("\"gradient\" needs [\"#top\", \"#bottom\"]");
            return -1;
        }
        return si;
    }
    bool is_shape = false;
    if ((v = SG_GET(o, "rect")) != nullptr) { s.verb = SGV_RECT; is_shape = true; if (!sg_exprs(v, s.ex, 4, "\"rect\"")) return -1; }
    else if ((v = SG_GET(o, "circle")) != nullptr) { s.verb = SGV_CIRCLE; is_shape = true; if (!sg_exprs(v, s.ex, 3, "\"circle\"")) return -1; }
    else if ((v = SG_GET(o, "line")) != nullptr) { s.verb = SGV_LINE; is_shape = true; if (!sg_exprs(v, s.ex, 4, "\"line\"")) return -1; }
    else if ((v = SG_GET(o, "pixel")) != nullptr) { s.verb = SGV_PIXEL; is_shape = true; if (!sg_exprs(v, s.ex, 2, "\"pixel\"")) return -1; }
    else if ((v = SG_GET(o, "text")) != nullptr) {
        s.verb = SGV_TEXT;
        is_shape = true;
        const cJSON *t0 = cJSON_IsArray(v) ? cJSON_GetArrayItem(v, 0) : nullptr;
        if (!cJSON_IsString(t0) || cJSON_GetArraySize(v) < 3) { sg_err("\"text\" needs [\"text\", x, y]"); return -1; }
        s.tmpl = sg_tmpl_compile(t0->valuestring);
        if (s.tmpl < 0) return -1;
        s.ex[0] = sg_expr_item(cJSON_GetArrayItem(v, 1));
        s.ex[1] = sg_expr_item(cJSON_GetArrayItem(v, 2));
        if (s.ex[0] < 0 || s.ex[1] < 0) return -1;
        const cJSON *sz = SG_GET(o, "size");
        if (cJSON_IsNumber(sz)) { int z = (int)sz->valuedouble; s.size = (uint8_t)(z < 1 ? 1 : (z > 4 ? 4 : z)); }
        const cJSON *al = SG_GET(o, "align");
        if (cJSON_IsString(al) && al->valuestring) s.align = !strcmp(al->valuestring, "center") ? 1 : (!strcmp(al->valuestring, "right") ? 2 : 0);
    } else if ((v = SG_GET(o, "sprite")) != nullptr) {
        s.verb = SGV_SPRITE;
        is_shape = false;
        if (!cJSON_IsString(v)) { sg_err("\"sprite\" needs a sprite name"); return -1; }
        s.kind = (int16_t)sg_spr_find(v->valuestring);
        if (s.kind < 0) { sg_err("unknown sprite", v->valuestring); return -1; }
        if (!sg_exprs(SG_GET(o, "at"), s.ex, 2, "\"at\"")) return -1;
        const cJSON *sc = SG_GET(o, "scale");
        if (cJSON_IsNumber(sc)) { int z = (int)sc->valuedouble; s.size = (uint8_t)(z < 1 ? 1 : (z > 8 ? 8 : z)); }
        const cJSON *fl = SG_GET(o, "flip");
        if (cJSON_IsTrue(fl)) s.flags |= 2;
    }
    if (s.verb == SGV_RECT || s.verb == SGV_CIRCLE || s.verb == SGV_LINE || s.verb == SGV_PIXEL || s.verb == SGV_TEXT) {
        if (!sg_color_item(SG_GET(o, "color"), &s.col, &s.colx)) return -1;
        const cJSON *fl = SG_GET(o, "fill");
        if (cJSON_IsFalse(fl)) s.flags &= (uint8_t)~1;
        return si;
    }
    if (s.verb == SGV_SPRITE) return si;
    (void)is_shape;

    if ((v = SG_GET(o, "set")) != nullptr) {
        s.verb = SGV_SET;
        if (!cJSON_IsObject(v)) { sg_err("\"set\" needs an object {name: expression}"); return -1; }
        if (!sg_assigns(v, &s, false)) return -1;
        return si;
    }
    if ((v = SG_GET(o, "fill")) != nullptr) {
        s.verb = SGV_FILL;
        if (!sg_color_item(v, &s.col, &s.colx)) return -1;
        return si;
    }
    sg_err("unknown command", (o->child && o->child->string) ? o->child->string : "{}");
    return -1;
}

static int sg_list(const cJSON *arr) {
    if (!arr) return -1;
    if (!cJSON_IsArray(arr)) { sg_err("expected a list [ ... ]"); return -1; }
    int n = cJSON_GetArraySize(arr);
    if (n == 0) return -1;
    if (sg_depth_c >= SG_MAX_DEPTH) { sg_err("statements are nested too deep"); return -1; }
    if (sgg->nl >= SG_MAXLISTS || sgg->ni + n > SG_MAXL) { sg_err("game is too large (lists)"); return -1; }
    int id = sgg->nl++;
    sgg->lists[id].first = sgg->ni;
    sgg->lists[id].n = n;
    sgg->ni += n;
    sg_depth_c++;
    for (int i = 0; i < n; i++) {
        int si = sg_stmt(cJSON_GetArrayItem(arr, i));
        sgg->items[sgg->lists[id].first + i] = si;
        if (si < 0) { sg_depth_c--; return -1; }
    }
    sg_depth_c--;
    return id;
}

static void sg_scene_body(const cJSON *src, SgScene *s) {
    s->on_start = sg_list(SG_GET(src, "on_start")); if (sgg->failed) return;
    s->on_tick = sg_list(SG_GET(src, "on_tick")); if (sgg->failed) return;
    s->on_a = sg_list(SG_GET(src, "on_a")); if (sgg->failed) return;
    s->on_b = sg_list(SG_GET(src, "on_b")); if (sgg->failed) return;
    s->on_a_hold = sg_list(SG_GET(src, "on_a_hold")); if (sgg->failed) return;
    s->on_b_hold = sg_list(SG_GET(src, "on_b_hold")); if (sgg->failed) return;
    s->on_shake = sg_list(SG_GET(src, "on_shake")); if (sgg->failed) return;
    s->back = sg_list(SG_GET(src, "back")); if (sgg->failed) return;
    s->draw = sg_list(SG_GET(src, "draw"));
}
static void sg_scene_reset(SgScene *s, const char *name) {
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%.19s", name);
    s->on_start = s->on_tick = s->on_a = s->on_b = s->on_a_hold = s->on_b_hold = s->on_shake = s->draw = s->back = -1;
}

static bool sg_compile(const cJSON *root) {
    for (int i = 0; i < SGB_COUNT; i++) sg_var_get(SG_BUILTIN_NAMES[i], true);

    const cJSON *nm = SG_GET(root, "name");
    snprintf(sgg->title, sizeof(sgg->title), "%.31s", (cJSON_IsString(nm) && nm->valuestring) ? nm->valuestring : "Game");
    sgg->vw = M5.Display.width();
    sgg->vh = M5.Display.height();
    const cJSON *scr = SG_GET(root, "screen");
    if (cJSON_IsArray(scr) && cJSON_GetArraySize(scr) >= 2 && cJSON_IsNumber(cJSON_GetArrayItem(scr, 0)) && cJSON_IsNumber(cJSON_GetArrayItem(scr, 1))) {
        int w = (int)cJSON_GetArrayItem(scr, 0)->valuedouble, h = (int)cJSON_GetArrayItem(scr, 1)->valuedouble;
        sgg->vw = w < 32 ? 32 : (w > 320 ? 320 : w);
        sgg->vh = h < 32 ? 32 : (h > 320 ? 320 : h);
    }
    const cJSON *fp = SG_GET(root, "fps");
    int fps = cJSON_IsNumber(fp) ? (int)fp->valuedouble : 30;
    sgg->fps = fps < 5 ? 5 : (fps > 60 ? 60 : fps);
    sgg->bg = 0x000000;
    int32_t k85_bgx = -1;   // K85SCR2: the background must be a constant color
    sg_color_item(SG_GET(root, "background"), &sgg->bg, &k85_bgx);
    sgg->score_var = 0;

    const cJSON *vars = SG_GET(root, "vars");
    if (cJSON_IsObject(vars)) {
        for (const cJSON *c = vars->child; c && !sgg->failed; c = c->next) {
            if (!c->string) continue;
            int vi = sg_var_get(c->string, true);
            if (vi < 0) break;
            if (cJSON_IsNumber(c)) sgg->vars[vi] = c->valuedouble;
            else if (cJSON_IsBool(c)) sgg->vars[vi] = cJSON_IsTrue(c) ? 1.0 : 0.0;
        }
    }
    sgg->var_i = sg_var_get("i", true);
    const cJSON *sv = SG_GET(root, "score_var");
    sgg->score_var = sg_var_get((cJSON_IsString(sv) && sv->valuestring) ? sv->valuestring : "score", true);
    const cJSON *hs = SG_GET(root, "hiscore");
    sgg->hiscore_on = !cJSON_IsFalse(hs);
    if (sgg->failed) return false;

    // sprites
    const cJSON *sps = SG_GET(root, "sprites");
    if (cJSON_IsObject(sps)) {
        for (const cJSON *c = sps->child; c && !sgg->failed; c = c->next) {
            if (!c->string) continue;
            if (sgg->nspr >= SG_MAXSPR) { sg_err("too many sprites"); break; }
            const cJSON *rows = SG_GET(c, "rows");
            const cJSON *pal = SG_GET(c, "palette");
            if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) == 0) { sg_err("a sprite needs \"rows\"", c->string); break; }
            int h = cJSON_GetArraySize(rows);
            if (h > 32) h = 32;
            int w = 1;
            for (int y = 0; y < h; y++) {
                const cJSON *r = cJSON_GetArrayItem(rows, y);
                if (cJSON_IsString(r) && r->valuestring) { int l = (int)strlen(r->valuestring); if (l > w) w = l; }
            }
            if (w > 32) w = 32;
            SgSprite &spr = sgg->spr[sgg->nspr];
            snprintf(spr.name, sizeof(spr.name), "%.19s", c->string);
            spr.w = w;
            spr.h = h;
            spr.px = (int32_t *)sg_alloc(sizeof(int32_t) * (size_t)w * (size_t)h);
            if (!spr.px) { sg_err("out of memory (sprites)"); break; }
            for (int y = 0; y < h; y++) {
                const cJSON *r = cJSON_GetArrayItem(rows, y);
                const char *rs = (cJSON_IsString(r) && r->valuestring) ? r->valuestring : "";
                int rl = (int)strlen(rs);
                for (int x = 0; x < w; x++) {
                    char ch = x < rl ? rs[x] : '.';
                    int32_t col = -1;
                    if (ch != '.' && ch != ' ') {
                        col = 0xFFFFFF;
                        char key[2] = { ch, 0 };
                        const cJSON *pc = cJSON_IsObject(pal) ? SG_GET(pal, key) : nullptr;
                        uint32_t cv = 0;
                        if (cJSON_IsString(pc) && pc->valuestring && sg_color_const(pc->valuestring, &cv)) col = (int32_t)cv;
                        else if (cJSON_IsNumber(pc)) col = (int32_t)((uint32_t)pc->valuedouble & 0xFFFFFF);
                    }
                    spr.px[y * w + x] = col;
                }
            }
            sgg->nspr++;
        }
    }

    // melodies
    const cJSON *mls = SG_GET(root, "melodies");
    if (cJSON_IsObject(mls)) {
        for (const cJSON *c = mls->child; c && !sgg->failed; c = c->next) {
            if (!c->string || !cJSON_IsArray(c)) continue;
            if (sgg->nmel >= SG_MAXMEL) { sg_err("too many melodies"); break; }
            SgMel &m = sgg->mel[sgg->nmel];
            snprintf(m.name, sizeof(m.name), "%.19s", c->string);
            m.n = 0;
            int cnt = cJSON_GetArraySize(c);
            for (int i = 0; i < cnt && m.n < SG_MAXNOTES; i++) {
                const cJSON *note = cJSON_GetArrayItem(c, i);
                if (!cJSON_IsArray(note) || cJSON_GetArraySize(note) < 2) continue;
                const cJSON *hz = cJSON_GetArrayItem(note, 0), *ms = cJSON_GetArrayItem(note, 1);
                if (!cJSON_IsNumber(hz) || !cJSON_IsNumber(ms)) continue;
                m.hz[m.n] = (uint16_t)(hz->valuedouble < 0 ? 0 : (hz->valuedouble > 12000 ? 12000 : hz->valuedouble));
                m.ms[m.n] = (uint16_t)(ms->valuedouble < 10 ? 10 : (ms->valuedouble > 5000 ? 5000 : ms->valuedouble));
                m.n++;
            }
            sgg->nmel++;
        }
    }
    if (sgg->failed) return false;

    // entities: register the names first (bodies may refer to each other), then compile the bodies
    const cJSON *ents = SG_GET(root, "entities");
    if (cJSON_IsObject(ents)) {
        for (const cJSON *c = ents->child; c; c = c->next) {
            if (!c->string) continue;
            if (sgg->nkinds >= SG_MAXKINDS) { sg_err("too many entity kinds"); return false; }
            SgKind &k = sgg->kinds[sgg->nkinds++];
            memset(&k, 0, sizeof(k));
            snprintf(k.name, sizeof(k.name), "%.19s", c->string);
            for (int f = 0; f < SGFLD_COUNT; f++) k.def[f] = -1;
            k.on_tick = k.draw = -1;
            k.auto_move = true;
        }
        int ki = 0;
        for (const cJSON *c = ents->child; c && !sgg->failed; c = c->next, ki++) {
            if (!c->string) continue;
            SgKind &k = sgg->kinds[ki];
            for (int f = 0; f < SGFLD_COUNT; f++) {
                const cJSON *fv = SG_GET(c, SG_FLD_NAMES[f]);
                if (fv) { k.def[f] = sg_expr_item(fv); if (k.def[f] < 0) return false; }
            }
            const cJSON *au = SG_GET(c, "auto");
            if (cJSON_IsFalse(au)) k.auto_move = false;
            k.on_tick = sg_list(SG_GET(c, "on_tick")); if (sgg->failed) return false;
            k.draw = sg_list(SG_GET(c, "draw"));
        }
    }
    if (sgg->failed) return false;

    // scenes
    const cJSON *scs = SG_GET(root, "scenes");
    if (cJSON_IsObject(scs)) {
        for (const cJSON *c = scs->child; c; c = c->next) {
            if (!c->string) continue;
            if (sgg->nscenes >= SG_MAXSCENES) { sg_err("too many scenes"); return false; }
            sg_scene_reset(&sgg->scenes[sgg->nscenes++], c->string);
        }
        int si = 0;
        for (const cJSON *c = scs->child; c && !sgg->failed; c = c->next, si++) {
            if (!c->string) continue;
            sg_scene_body(c, &sgg->scenes[si]);
        }
    } else {
        sg_scene_reset(&sgg->scenes[sgg->nscenes++], "main");
        sg_scene_body(root, &sgg->scenes[0]);
    }
    if (sgg->failed) return false;
    if (sgg->nscenes == 0) { sg_err("the game has no scenes"); return false; }
    sgg->start_scene = 0;
    const cJSON *st = SG_GET(root, "start");
    if (cJSON_IsString(st) && st->valuestring) {
        sgg->start_scene = sg_scene_find(st->valuestring);
        if (sgg->start_scene < 0) { sg_err("unknown start scene", st->valuestring); return false; }
    }
    memcpy(sgg->init, sgg->vars, sizeof(double) * (size_t)sgg->nvars);
    return !sgg->failed;
}

// ---------- running ----------
static void sg_tone(int hz, int ms) {
    if (hz < 20 || hz > 12000 || ms < 1) return;
    if (g_config.sound_muted) return;
    M5.Speaker.tone((float)hz, (uint32_t)ms);
}

static void sg_exec_list(int id, SgEnt *self);

static void sg_spawn(const SgStmt &s) {
    int slot = -1;
    for (int i = 0; i < sgg->nent_high; i++) if (!sgg->ents[i].alive) { slot = i; break; }
    if (slot < 0) { if (sgg->nent_high >= SG_MAXENT) return; slot = sgg->nent_high++; }
    SgEnt &e = sgg->ents[slot];
    memset(&e, 0, sizeof(e));
    e.kind = (int8_t)s.kind;
    e.alive = true;
    const SgKind &k = sgg->kinds[s.kind];
    for (int f = 0; f < SGFLD_COUNT; f++) if (k.def[f] >= 0) e.f[f] = sg_ev(k.def[f], nullptr);
    for (int i = 0; i < s.asg_n; i++) {
        const SgAsg &a = sgg->asg[s.asg_first + i];
        double val = sg_ev(a.expr, &e);
        if (a.target >= 0) sgg->vars[a.target] = val;
        else e.f[-(a.target + 1)] = val;
    }
}

static uint32_t sg_lerp(uint32_t a, uint32_t b, int t256) {
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg2 = (b >> 8) & 0xFF, bb = b & 0xFF;
    return ((uint32_t)(ar + (br - ar) * t256 / 256) << 16) | ((uint32_t)(ag + (bg2 - ag) * t256 / 256) << 8) | (uint32_t)(ab + (bb - ab) * t256 / 256);
}

static void sg_exec_stmt(int si, SgEnt *self) {
    if (++sgg->ops > SG_OPS_MAX) return;
    const SgStmt &s = sgg->st[si];
    M5Canvas *c = sgg->cv;
    switch (s.verb) {
        case SGV_SET:
            for (int i = 0; i < s.asg_n; i++) {
                const SgAsg &a = sgg->asg[s.asg_first + i];
                double val = sg_ev(a.expr, self);
                if (a.target >= 0) sgg->vars[a.target] = val;
                else if (self) self->f[-(a.target + 1)] = val;
            }
            break;
        case SGV_IF:
            if (sg_ev(s.cond, self) != 0.0) sg_exec_list(s.blk_a, self);
            else sg_exec_list(s.blk_b, self);
            break;
        case SGV_REPEAT: {
            int n = (int)sg_ev(s.cond, self);
            if (n > 256) n = 256;
            for (int i = 0; i < n && sgg->ops <= SG_OPS_MAX; i++) {
                sgg->vars[sgg->var_i] = (double)i;
                sg_exec_list(s.blk_a, self);
            }
            break;
        }
        case SGV_EACH: {
            int hi = sgg->nent_high;
            for (int i = 0; i < hi && sgg->ops <= SG_OPS_MAX; i++) {
                SgEnt *e = &sgg->ents[i];
                if (e->alive && e->kind == s.kind) sg_exec_list(s.blk_a, e);
            }
            break;
        }
        case SGV_SPAWN: sg_spawn(s); break;
        case SGV_REMOVE: if (self) self->alive = false; break;
        case SGV_REMOVE_ALL:
            for (int i = 0; i < sgg->nent_high; i++) if (sgg->ents[i].kind == s.kind) sgg->ents[i].alive = false;
            break;
        case SGV_GOTO: sgg->pending_scene = s.kind; break;
        case SGV_END:
            sgg->ended = s.kind;
            if (s.tmpl >= 0) sg_tmpl_eval(s.tmpl, self, sgg->end_msg, sizeof(sgg->end_msg));
            else sgg->end_msg[0] = 0;
            break;
        case SGV_PLAY:
            if (s.kind >= 0) { sgg->mel_idx = s.kind; sgg->mel_pos = 0; sgg->mel_next = 0; }
            else sg_tone(sg_iv(s.ex[0], self), sg_iv(s.ex[1], self));
            break;
        case SGV_FILL: c->fillScreen(sg_col(s.col, s.colx, self)); break;
        case SGV_GRADIENT:
            for (int y = 0; y < sgg->vh; y++) c->drawFastHLine(0, y, sgg->vw, sg_lerp(s.col, s.col2, y * 256 / (sgg->vh > 1 ? sgg->vh - 1 : 1)));
            break;
        case SGV_RECT: {
            int x = sg_iv(s.ex[0], self), y = sg_iv(s.ex[1], self), w = sg_iv(s.ex[2], self), h = sg_iv(s.ex[3], self);
            uint32_t col = sg_col(s.col, s.colx, self);
            if (s.flags & 1) c->fillRect(x, y, w, h, col); else c->drawRect(x, y, w, h, col);
            break;
        }
        case SGV_CIRCLE: {
            int x = sg_iv(s.ex[0], self), y = sg_iv(s.ex[1], self), r = sg_iv(s.ex[2], self);
            uint32_t col = sg_col(s.col, s.colx, self);
            if (s.flags & 1) c->fillCircle(x, y, r, col); else c->drawCircle(x, y, r, col);
            break;
        }
        case SGV_LINE:
            c->drawLine(sg_iv(s.ex[0], self), sg_iv(s.ex[1], self), sg_iv(s.ex[2], self), sg_iv(s.ex[3], self), sg_col(s.col, s.colx, self));
            break;
        case SGV_PIXEL:
            c->drawPixel(sg_iv(s.ex[0], self), sg_iv(s.ex[1], self), sg_col(s.col, s.colx, self));
            break;
        case SGV_TEXT: {
            char buf[96];
            sg_tmpl_eval(s.tmpl, self, buf, sizeof(buf));
            int x = sg_iv(s.ex[0], self), y = sg_iv(s.ex[1], self);
            int wpx = (int)strlen(buf) * 6 * s.size;
            if (s.align == 1) x -= wpx / 2; else if (s.align == 2) x -= wpx;
            c->setTextSize(s.size);
            c->setTextColor(sg_col(s.col, s.colx, self));
            c->setCursor(x, y);
            c->print(buf);
            break;
        }
        case SGV_SPRITE: {
            const SgSprite &sp = sgg->spr[s.kind];
            int x0 = sg_iv(s.ex[0], self), y0 = sg_iv(s.ex[1], self);
            int sc = s.size;
            for (int y = 0; y < sp.h; y++) {
                for (int x = 0; x < sp.w; x++) {
                    int32_t col = sp.px[y * sp.w + ((s.flags & 2) ? (sp.w - 1 - x) : x)];
                    if (col < 0) continue;
                    if (sc == 1) c->drawPixel(x0 + x, y0 + y, (uint32_t)col);
                    else c->fillRect(x0 + x * sc, y0 + y * sc, sc, sc, (uint32_t)col);
                }
            }
            break;
        }
    }
}

static void sg_exec_list(int id, SgEnt *self) {
    if (id < 0) return;
    if (sgg->depth >= SG_MAX_DEPTH) return;
    sgg->depth++;
    const SgList &L = sgg->lists[id];
    for (int i = 0; i < L.n; i++) {
        if (sgg->pending_scene >= 0 || sgg->ended >= 0 || sgg->ops > SG_OPS_MAX) break;
        sg_exec_stmt(sgg->items[L.first + i], self);
    }
    sgg->depth--;
}

static void sg_enter_scene(int idx) {
    sgg->cur_scene = idx;
    sgg->pending_scene = -1;
    sgg->vars[SGB_TIME] = 0.0;
    sg_exec_list(sgg->scenes[idx].on_start, nullptr);
}
static void sg_apply_pending(void) {
    for (int guard = 0; sgg->pending_scene >= 0 && guard < 8; guard++) sg_enter_scene(sgg->pending_scene);
    sgg->pending_scene = -1;
}
static void sg_restart(void) {
    memset(sgg->ents, 0, sizeof(sgg->ents));
    sgg->nent_high = 0;
    memcpy(sgg->vars, sgg->init, sizeof(double) * (size_t)sgg->nvars);
    sgg->vars[SGB_W] = (double)sgg->vw;
    sgg->vars[SGB_H] = (double)sgg->vh;
    sgg->vars[SGB_FRAME] = 0.0;
    sgg->vars[SGB_HI] = (double)sgg->hi;
    sgg->ended = -1;
    sgg->end_msg[0] = 0;
    sgg->pending_scene = -1;
    sgg->mel_idx = -1;
    sgg->ops = 0;
    sgg->depth = 0;
    sg_enter_scene(sgg->start_scene);
    sg_apply_pending();
}

static void sg_update_entities(double dt) {
    int hi = sgg->nent_high;
    for (int i = 0; i < hi; i++) {
        SgEnt &e = sgg->ents[i];
        if (!e.alive) continue;
        const SgKind &k = sgg->kinds[e.kind];
        if (k.auto_move) { e.f[SGFLD_X] += e.f[SGFLD_VX] * dt; e.f[SGFLD_Y] += e.f[SGFLD_VY] * dt; }
        if (k.on_tick >= 0) sg_exec_list(k.on_tick, &e);
        if (e.f[SGFLD_LIFE] > 0.0) { e.f[SGFLD_LIFE] -= dt; if (e.f[SGFLD_LIFE] <= 0.0) e.alive = false; }
        if (e.f[SGFLD_X] < -64.0 || e.f[SGFLD_X] > sgg->vw + 64.0 || e.f[SGFLD_Y] < -64.0 || e.f[SGFLD_Y] > sgg->vh + 64.0) e.alive = false;
    }
    while (sgg->nent_high > 0 && !sgg->ents[sgg->nent_high - 1].alive) sgg->nent_high--;
}

static void sg_render(void) {
    sgg->cv->fillScreen(sgg->bg);
    const SgScene &sc = sgg->scenes[sgg->cur_scene];
    sg_exec_list(sc.back, nullptr);
    for (int i = 0; i < sgg->nent_high; i++) {
        SgEnt &e = sgg->ents[i];
        if (!e.alive) continue;
        const SgKind &k = sgg->kinds[e.kind];
        if (k.draw >= 0) sg_exec_list(k.draw, &e);
    }
    sg_exec_list(sc.draw, nullptr);
}

// ---------- high score and screens ----------
static void sg_hi_path(const char *base, char *out, size_t cap) { snprintf(out, cap, "%s/save/%.40s.hi", K85_JSON_GAMES_DIR, base); }
static int sg_hi_load(const char *base) {
    char p[120];
    sg_hi_path(base, p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char buf[24] = { 0 };
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return atoi(buf);
}
static void sg_hi_save(const char *base, int v) {
    mkdir(K85_JSON_GAMES_DIR "/save", 0755);
    char p[120];
    sg_hi_path(base, p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%d\n", v);
    fclose(f);
}
static bool sg_hi_commit(const char *base) {
    int sc = (int)sgg->vars[sgg->score_var];
    if (sgg->hiscore_on && sc > sgg->hi) { sgg->hi = sc; sg_hi_save(base, sc); return true; }
    return false;
}

static void sg_wrap_print(const char *text, int x, int y, int cols, int max_lines) {
    char line[48];
    const char *p = text;
    int ln = 0;
    while (*p && ln < max_lines) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < cols && n < 46) n++;
        if (p[n] && p[n] != '\n') { int sp = n; while (sp > 0 && p[sp] != ' ') sp--; if (sp > 0) n = sp; }
        memcpy(line, p, (size_t)n);
        line[n] = 0;
        M5.Display.setCursor(x, y + ln * 10);
        M5.Display.print(line);
        ln++;
        p += n;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
}

static void sg_error_screen(const char *title, const char *msg) {
    M5.Display.fillScreen(0x000000);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xFF6666, 0x000000);
    M5.Display.setCursor(4, 4);
    M5.Display.print(title);
    M5.Display.setTextColor(0xFFFFFF, 0x000000);
    int cols = (M5.Display.width() - 8) / 6;
    if (cols < 10) cols = 10;
    if (cols > 44) cols = 44;
    sg_wrap_print(msg, 4, 20, cols, 16);
    M5.Display.setTextColor(0x888888, 0x000000);
    M5.Display.setCursor(4, M5.Display.height() - 12);
    M5.Display.print("A+B = back");
    wait_ab_exit();
}

static bool sg_end_screen(const char *base) {
    bool fresh = sg_hi_commit(base);
    int W = M5.Display.width(), H = M5.Display.height();
    M5.Display.fillScreen(0x000000);
    const char *ttl = sgg->ended == 0 ? "YOU WIN" : (sgg->ended == 1 ? "GAME OVER" : "THE END");
    uint32_t tc = sgg->ended == 0 ? 0x66FF88 : (sgg->ended == 1 ? 0xFF6677 : 0xBBBBBB);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(tc, 0x000000);
    M5.Display.setCursor((W - (int)strlen(ttl) * 12) / 2, H / 2 - 50);
    M5.Display.print(ttl);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xFFFFFF, 0x000000);
    int y = H / 2 - 22;
    if (sgg->end_msg[0]) {
        M5.Display.setCursor((W - (int)strlen(sgg->end_msg) * 6) / 2, y);
        M5.Display.print(sgg->end_msg);
        y += 14;
    }
    char line[40];
    snprintf(line, sizeof(line), "Score: %d", (int)sgg->vars[sgg->score_var]);
    M5.Display.setCursor((W - (int)strlen(line) * 6) / 2, y);
    M5.Display.print(line);
    y += 12;
    if (sgg->hiscore_on) {
        snprintf(line, sizeof(line), fresh ? "New best: %d" : "Best: %d", sgg->hi);
        M5.Display.setTextColor(0xFFDD66, 0x000000);
        M5.Display.setCursor((W - (int)strlen(line) * 6) / 2, y);
        M5.Display.print(line);
    }
    M5.Display.setTextColor(0x999999, 0x000000);
    const char *foot = "B = again   A+B = exit";
    M5.Display.setCursor((W - (int)strlen(foot) * 6) / 2, H - 14);
    M5.Display.print(foot);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) return true;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void sg_free(void) {
    if (!sgg) return;
    for (int i = 0; i < sgg->nspr; i++) if (sgg->spr[i].px) heap_caps_free(sgg->spr[i].px);
    if (sgg->cv) { sgg->cv->deleteSprite(); delete sgg->cv; sgg->cv = nullptr; }
    heap_caps_free(sgg->nodes); heap_caps_free(sgg->args); heap_caps_free(sgg->st); heap_caps_free(sgg->items);
    heap_caps_free(sgg->lists); heap_caps_free(sgg->asg); heap_caps_free(sgg->tmpl); heap_caps_free(sgg->parts);
    heap_caps_free(sgg->pool);
    heap_caps_free(sgg);
    sgg = nullptr;
}

static void run_script_game(cJSON *root, const char *base) {
    sgg = (SgGame *)sg_alloc(sizeof(SgGame));
    if (!sgg) { k85_show_message("Out of memory\nA+B=back"); wait_ab_exit(); return; }
    sgg->nodes = (SgNode *)sg_alloc(sizeof(SgNode) * SG_MAXN);
    sgg->args = (int32_t *)sg_alloc(sizeof(int32_t) * SG_MAXARGS);
    sgg->st = (SgStmt *)sg_alloc(sizeof(SgStmt) * SG_MAXS);
    sgg->items = (int32_t *)sg_alloc(sizeof(int32_t) * SG_MAXL);
    sgg->lists = (SgList *)sg_alloc(sizeof(SgList) * SG_MAXLISTS);
    sgg->asg = (SgAsg *)sg_alloc(sizeof(SgAsg) * SG_MAXASG);
    sgg->tmpl = (SgTmpl *)sg_alloc(sizeof(SgTmpl) * SG_MAXTMPL);
    sgg->parts = (SgPart *)sg_alloc(sizeof(SgPart) * SG_MAXPART);
    sgg->pool = (char *)sg_alloc(SG_POOL);
    sg_depth_c = 0;
    if (!sgg->nodes || !sgg->args || !sgg->st || !sgg->items || !sgg->lists || !sgg->asg || !sgg->tmpl || !sgg->parts || !sgg->pool) {
        sg_free();
        k85_show_message("Out of memory\nA+B=back");
        wait_ab_exit();
        return;
    }
    if (!sg_compile(root)) {
        char m[200];
        snprintf(m, sizeof(m), "%s", sgg->failed ? sgg->err : "cannot read the game");
        sg_free();
        sg_error_screen("Script error", m);
        return;
    }
    sgg->cv = new M5Canvas(&M5.Display);
    bool cv_ok = false;
    if (sgg->cv) {
        sgg->cv->setColorDepth(16);
        sgg->cv->setPsram(true);
        cv_ok = sgg->cv->createSprite(sgg->vw, sgg->vh) != nullptr;
    }
    if (!cv_ok) { sg_free(); k85_show_message("No memory for the\nscreen of the game\nA+B=back"); wait_ab_exit(); return; }

    sgg->hi = sg_hi_load(base);
    int W = M5.Display.width(), H = M5.Display.height();
    bool native = (sgg->vw == W && sgg->vh == H);
    float zx = (float)W / (float)sgg->vw, zy = (float)H / (float)sgg->vh;
    float zoom = zx < zy ? zx : zy;
    M5.Display.fillScreen(0x000000);
    sg_restart();

    const uint32_t period_ms = 1000u / (uint32_t)sgg->fps;
    int64_t last_us = esp_timer_get_time();
    bool pend_a = false, pend_b = false, pend_ah = false, pend_bh = false, pend_shake = false;
    uint32_t last_shake_ms = 0;
    bool running = true;

    while (running) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); sg_hi_commit(base); break; }
        if (k85_btn_a_pressed()) pend_a = true;
        if (k85_btn_b_pressed()) pend_b = true;
        if (M5.BtnA.wasHold() && !M5.BtnB.isPressed()) pend_ah = true;
        if (M5.BtnB.wasHold() && !M5.BtnA.isPressed()) pend_bh = true;
        sgg->vars[SGB_BTN_A] = M5.BtnA.isPressed() ? 1.0 : 0.0;
        sgg->vars[SGB_BTN_B] = M5.BtnB.isPressed() ? 1.0 : 0.0;

        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_us) >= (int64_t)period_ms * 1000) {
            double dt = (double)(now_us - last_us) / 1e6;
            if (dt > 0.1) dt = 0.1;
            last_us = now_us;
            uint32_t now_ms = (uint32_t)(now_us / 1000);

            float ax = 0.0f, ay = 0.0f, az = 1.0f;
            M5.Imu.getAccel(&ax, &ay, &az);
            sgg->vars[SGB_TILT_X] = (double)ax;
            sgg->vars[SGB_TILT_Y] = (double)ay;
            float mag = sqrtf(ax * ax + ay * ay + az * az);
            if (fabsf(mag - 1.0f) > 0.9f && (now_ms - last_shake_ms) > 600) { last_shake_ms = now_ms; pend_shake = true; }

            sgg->vars[SGB_DT] = dt;
            sgg->vars[SGB_TIME] += dt;
            sgg->vars[SGB_FRAME] += 1.0;
            sgg->ops = 0;
            sgg->depth = 0;
            int ci = sgg->cur_scene;
            if (pend_a) sg_exec_list(sgg->scenes[ci].on_a, nullptr);
            if (pend_b) sg_exec_list(sgg->scenes[ci].on_b, nullptr);
            if (pend_ah) sg_exec_list(sgg->scenes[ci].on_a_hold, nullptr);
            if (pend_bh) sg_exec_list(sgg->scenes[ci].on_b_hold, nullptr);
            if (pend_shake) sg_exec_list(sgg->scenes[ci].on_shake, nullptr);
            pend_a = pend_b = pend_ah = pend_bh = pend_shake = false;
            sg_apply_pending();
            if (sgg->ended < 0) {
                sg_exec_list(sgg->scenes[sgg->cur_scene].on_tick, nullptr);
                if (sgg->ended < 0) sg_update_entities(dt);
                sg_apply_pending();
            }
            if (sgg->ended < 0) {
                sg_render();
                if (native) sgg->cv->pushSprite(&M5.Display, 0, 0);
                else sgg->cv->pushRotateZoom(&M5.Display, (float)W / 2.0f, (float)H / 2.0f, 0.0f, zoom, zoom);
            }

            if (sgg->mel_idx >= 0 && now_ms >= sgg->mel_next) {
                const SgMel &m = sgg->mel[sgg->mel_idx];
                if (sgg->mel_pos >= m.n) sgg->mel_idx = -1;
                else { sg_tone(m.hz[sgg->mel_pos], m.ms[sgg->mel_pos]); sgg->mel_next = now_ms + m.ms[sgg->mel_pos]; sgg->mel_pos++; }
            }

            if (sgg->ended >= 0) {
                if (sg_end_screen(base)) {
                    M5.Display.fillScreen(0x000000);
                    sg_restart();
                    last_us = esp_timer_get_time();
                } else {
                    running = false;
                }
            }
            vTaskDelay(1);
        } else {
            vTaskDelay(pdMS_TO_TICKS(3));
        }
    }
    sg_free();
}

// ---------- loader: parse into PSRAM, detect the game type ----------
static void *sg_psram_malloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
static bool sg_jhas(const cJSON *r, const char *k) { return cJSON_GetObjectItemCaseSensitive(r, k) != nullptr; }

static void run_game_from_json(char *json_buf, const char *base) {
    cJSON_Hooks hooks;
    hooks.malloc_fn = sg_psram_malloc;
    hooks.free_fn = free;
    cJSON_InitHooks(&hooks);              // big games: the whole tree lives in PSRAM
    cJSON *root = cJSON_Parse(json_buf);
    const char *ep = cJSON_GetErrorPtr();
    cJSON_InitHooks(nullptr);
    if (!root) {
        long off = (ep && ep >= json_buf) ? (long)(ep - json_buf) : -1;
        int line = 1, col = 1;
        for (long i = 0; i < off && json_buf[i]; i++) { if (json_buf[i] == '\n') { line++; col = 1; } else col++; }
        char snip[28] = { 0 };
        if (off >= 0) {
            int n = 0;
            for (long i = off; json_buf[i] && n < 24; i++, n++) snip[n] = (json_buf[i] == '\n' || json_buf[i] == '\r') ? ' ' : json_buf[i];
            snip[n] = 0;
        }
        char m[160];
        snprintf(m, sizeof(m), "Invalid JSON.\nline %d, column %d\nnear: %s", line, col, snip);
        sg_error_screen("JSON error", m);
        return;
    }

    cJSON *type_j = cJSON_GetObjectItem(root, "type");
    const char *tp = cJSON_IsString(type_j) ? type_j->valuestring : "";
    if (!strcmp(tp, "grid")) {
        run_grid_game(root);
    } else if (!strcmp(tp, "script") || !strcmp(tp, "graphic") || !strcmp(tp, "game") ||
               (!tp[0] && (sg_jhas(root, "scenes") || sg_jhas(root, "on_tick") || sg_jhas(root, "draw")))) {
        run_script_game(root, base);
    } else {
        run_quest_game(root);
    }
    cJSON_Delete(root);
}
void k85_run_json_interpreter(void) {
    sg_write_samples();   // K85SCR1
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

    struct stat gst;
    if (stat(path, &gst) != 0) {
        k85_show_message("Cannot open file\nA+B=back");
        wait_ab_exit();
        return;
    }
    if (gst.st_size > K85_JSON_MAX_BYTES) {   // K85SCR1: games up to 256 KB
        k85_show_message("Game file too large\n(max 256 KB)\nA+B=back");
        wait_ab_exit();
        return;
    }
    size_t fsize = (size_t)gst.st_size;
    char *buf = (char *)heap_caps_malloc(fsize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        k85_show_message("Out of memory\nA+B=back");
        wait_ab_exit();
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        heap_caps_free(buf);
        k85_show_message("Cannot open file\nA+B=back");
        wait_ab_exit();
        return;
    }
    size_t r = fread(buf, 1, fsize, f);
    fclose(f);
    buf[r] = 0;

    char base[48];
    snprintf(base, sizeof(base), "%.44s", names[idx]);
    size_t bl = strlen(base);
    if (bl > 5 && !strcmp(base + bl - 5, ".json")) base[bl - 5] = 0;

    run_game_from_json(buf, base);
    heap_caps_free(buf);
}