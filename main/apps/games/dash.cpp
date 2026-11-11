// k85OS "Dash": one-button runner. A = jump (hold = keep jumping), B = pause menu.
// A level is a text spec: letters = obstacle segments (k_segs), digits = flat columns.
#include "dash.h"
#include "config.h"
#include "theme.h"
#include "input.h"
#include "power.h"
#include "common.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

#define DASH_T          12      // tile size (px)
#define DASH_P          12      // cube size (px)
#define DASH_MAX_COLS   640
#define DASH_LEVELS     5
#define DASH_FRAME_MS   33
#define DASH_PARTS      10
#define DASH_CP_FRAMES  90      // ~3 s at 30 fps: practice mode drops a checkpoint diamond

static const float DASH_GRAV   = 0.6f;
static const float DASH_JUMP_V = 6.0f;
static const float DASH_VMAX   = 8.0f;

static const uint32_t DASH_C_WHITE  = 0xFFFFFF;
static const uint32_t DASH_C_DARK   = 0x101018;
static const uint32_t DASH_C_PIT    = 0x05070D;
static const uint32_t DASH_C_PANEL  = 0x15151F;
static const uint32_t DASH_C_BAR    = 0x2A2A36;
static const uint32_t DASH_C_BODY   = 0xFFE24A;
static const uint32_t DASH_C_ORANGE = 0xFF8A00;
static const uint32_t DASH_C_EYE    = 0x20C8FF;

// ---------------- level data ----------------
struct DashSeg { char key; const char *r0; const char *r1; const char *r2; };
// r0 = row on the ground, r1 / r2 = rows above it.  # block   ^ spike   v pit (row 0 only)   . empty
static const DashSeg k_segs[] = {
    { 'a', "^",       "",        "" },   // one spike
    { 'b', "^^",      "",        "" },   // two spikes
    { 'c', "^^^",     "",        "" },   // three spikes
    { 'd', "#",       "",        "" },   // 1-high block
    { 'e', "#",       "#",       "" },   // 2-high block
    { 'f', "##",      ".#",      "" },   // stairs
    { 'g', "vv",      "",        "" },   // pit 2
    { 'h', "vvv",     "",        "" },   // pit 3
    { 'i', "vvvv",    "",        "" },   // pit 4
    { 'k', "vvvvv",   "",        "" },   // pit 5
    { 'j', "#######", "....^..", "" },   // platform with a spike
    { 'm', "^.^",     "",        "" },   // two spikes with a gap
    { 'n', "###",     "",        "" },   // wide block
};

struct DashLevel {
    const char *name;
    const char *diff;
    int stars;
    uint32_t col;                 // difficulty colour
    float speed;                  // px per frame
    uint32_t sky, stripe, ground, line, block, edge;
    const char *key;              // high score key (best %)
    const char *spec;
};

static const DashLevel k_levels[DASH_LEVELS] = {
    { "First Steps", "EASY",   1, 0x3ADB4F, 2.4f, 0x0E2A47, 0x143556, 0x1B6E3A, 0x57F287, 0x1F9D55, 0x9CF5BE, "dash1",
      "93a7a7d7a7g7a7b7d7b7g7a6d6b7f7a6b7g7d6a6b7" },
    { "Spike Valley", "NORMAL", 2, 0xB5E61D, 2.7f, 0x241A47, 0x2D2058, 0x5B3A8C, 0xB48CFF, 0x6B47A8, 0xC9B0FF, "dash2",
      "93a6b6d6e6g5a6h6f6b6e5d6a6n6g6b5a6h6e5b6f6d5a5b6g6e6h6a6" },
    { "Pit Party", "HARD",     3, 0xFF9A1F, 3.0f, 0x2E1A10, 0x3A2214, 0x8A4B1B, 0xFFB35C, 0xB5651D, 0xFFD59E, "dash3",
      "93b5a5j6h5e5b6i5d5a5a6f5g5b5e6h5j6b5n5a5i6e5b5h5d5a6g5f5b6" },
    { "Block Rush", "HARDER",  4, 0xFF4D2E, 3.3f, 0x3A0E14, 0x4A1219, 0x8C1F2B, 0xFF6B6B, 0xB02A37, 0xFFA3A8, "dash4",
      "93c5b5h5e5m5i5a5b5j5d5c5g5n5b6a5i5f5c5h5b5e5m5j5a5c5i5b5d5h5a6" },
    { "Inferno", "INSANE",     5, 0xFF2BD6, 3.7f, 0x14001F, 0x1E0030, 0x4B0A6B, 0xFF2BD6, 0x6E1596, 0xFF9CF0, "dash5",
      "93c4b5k4m5j4i4c5a4b5e4h5c4m5k4b5j4i5c4g5b4h5k4c5m4b5j4i5c5k5a" },
};

static uint8_t s_grid[3][DASH_MAX_COLS];     // 0 empty, 1 block, 2 spike, 3 pit (row 0)
static int s_cols = 0;

static inline uint8_t dash_cell(int r, int c) {
    if (r < 0 || r > 2 || c < 0 || c >= s_cols) return 0;
    return s_grid[r][c];
}

static void dash_build(const char *spec) {
    memset(s_grid, 0, sizeof(s_grid));
    int col = 0;
    for (const char *p = spec; *p && col < DASH_MAX_COLS - 16; p++) {
        const char ch = *p;
        if (ch >= '0' && ch <= '9') { col += ch - '0'; continue; }
        for (size_t i = 0; i < sizeof(k_segs) / sizeof(k_segs[0]); i++) {
            if (k_segs[i].key != ch) continue;
            const char *rows[3] = { k_segs[i].r0, k_segs[i].r1, k_segs[i].r2 };
            const int w = (int)strlen(k_segs[i].r0);
            for (int r = 0; r < 3; r++) {
                const int rl = (int)strlen(rows[r]);
                for (int x = 0; x < w && col + x < DASH_MAX_COLS; x++) {
                    const char c = (x < rl) ? rows[r][x] : '.';
                    uint8_t v = 0;
                    if (c == '#') v = 1;
                    else if (c == '^') v = 2;
                    else if (c == 'v') v = 3;
                    s_grid[r][col + x] = v;
                }
            }
            col += w;
            break;
        }
    }
    s_cols = col + 8;       // flat run-out after the last obstacle
}

// ---------------- state ----------------
struct DashState {
    float wx, py, vy, ang;
    bool on_ground, dead;
    int buf, dead_f, pct, attempt;
    bool practice, cp_valid;
    int cp_col, cp_timer;
    float qx[DASH_PARTS], qy[DASH_PARTS], qvx[DASH_PARTS], qvy[DASH_PARTS];
};

static uint32_t s_rng = 12345;
static bool s_practice = false;     // hold B in the level list = practice mode (checkpoints)
static uint32_t dash_rand(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 8; }
static uint32_t dash_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void dash_reset(DashState &s, int GY) {
    s.wx = 0.0f; s.py = (float)GY - DASH_P; s.vy = 0.0f; s.ang = 0.0f;
    s.on_ground = true; s.dead = false; s.buf = 0; s.dead_f = 0; s.pct = 0;
}

// a checkpoint column: flat ground, 3 more clear columns ahead (a jump can always be timed from there)
static bool dash_cp_safe(int c) {
    if (c < 4) return false;
    for (int x = c; x <= c + 3; x++)
        for (int r = 0; r < 3; r++)
            if (dash_cell(r, x) != 0) return false;
    return true;
}

// after a crash: back to the last checkpoint diamond (practice) or to the start
static void dash_respawn(DashState &s, int GY, int PX) {
    dash_reset(s, GY);
    s.cp_timer = 0;
    if (s.practice && s.cp_valid) s.wx = (float)(s.cp_col * DASH_T - PX);
}

static void dash_burst(DashState &s, float cx, float cy) {
    for (int i = 0; i < DASH_PARTS; i++) {
        s.qx[i] = cx; s.qy[i] = cy;
        s.qvx[i] = (float)((int)(dash_rand() % 100) - 50) / 12.0f;
        s.qvy[i] = -(float)((int)(dash_rand() % 60)) / 10.0f - 1.0f;
    }
}

// ---------------- drawing ----------------
static void dash_ctext(lgfx::LGFXBase &g, int x, int w, const char *s, int y, int size, uint32_t color) {
    g.setTextSize((float)size);
    if (size > 1 && g.textWidth(s) > w - 4) g.setTextSize((float)(size - 1));
    const int tw = g.textWidth(s);
    g.setTextColor(color);
    g.drawString(s, x + (w - tw) / 2, y);
}

static void dash_rot_sq(lgfx::LGFXBase &g, float cx, float cy, float half, float rad, uint32_t color) {
    const float sn = sinf(rad), cs = cosf(rad);
    if (fabsf(sn) < 0.02f || fabsf(cs) < 0.02f) {          // 0 / 90 / 180 / 270 deg: still an axis-aligned square
        g.fillRect((int)lroundf(cx - half), (int)lroundf(cy - half), (int)lroundf(half * 2.0f), (int)lroundf(half * 2.0f), color);
        return;
    }
    static const int sx[4] = { -1, 1, 1, -1 };
    static const int sy[4] = { -1, -1, 1, 1 };
    int x[4], y[4];
    for (int i = 0; i < 4; i++) {
        const float px = (float)sx[i] * half, py = (float)sy[i] * half;
        x[i] = (int)lroundf(cx + px * cs - py * sn);
        y[i] = (int)lroundf(cy + px * sn + py * cs);
    }
    g.fillTriangle(x[0], y[0], x[1], y[1], x[2], y[2], color);
    g.fillTriangle(x[0], y[0], x[2], y[2], x[3], y[3], color);
}

static void dash_draw(lgfx::LGFXBase &g, int W, int H, int GY, int PX, int fin_x, const DashLevel &L, const DashState &s) {
    const int ox = (int)lroundf(s.wx);
    g.fillScreen(L.sky);
    const int po = (int)(s.wx * 0.3f) % 48;
    for (int x = -po; x < W; x += 48) g.fillRect(x, 0, 16, GY, L.stripe);
    g.fillRect(0, GY, W, H - GY, L.ground);
    g.fillRect(0, GY, W, 2, L.line);

    const int c0 = ox / DASH_T - 1;
    const int c1 = c0 + W / DASH_T + 3;
    for (int c = c0; c <= c1; c++) {
        const int x = c * DASH_T - ox;
        if (dash_cell(0, c) == 3) g.fillRect(x, GY, DASH_T, H - GY, DASH_C_PIT);
        for (int r = 0; r < 3; r++) {
            const uint8_t t = dash_cell(r, c);
            const int y = GY - (r + 1) * DASH_T;
            if (t == 1) {
                g.fillRect(x, y, DASH_T, DASH_T, L.block);
                g.drawRect(x, y, DASH_T, DASH_T, L.edge);
                g.drawRect(x + 3, y + 3, DASH_T - 6, DASH_T - 6, L.edge);
            } else if (t == 2) {
                g.fillTriangle(x + 1, y + DASH_T - 1, x + DASH_T / 2, y + 1, x + DASH_T - 2, y + DASH_T - 1, DASH_C_WHITE);
                g.drawTriangle(x + 1, y + DASH_T - 1, x + DASH_T / 2, y + 1, x + DASH_T - 2, y + DASH_T - 1, L.edge);
            }
        }
    }

    const int fx = fin_x - ox;                               // finish line
    if (fx > -DASH_T && fx < W) {
        for (int yy = 0; yy < GY; yy += 6) {
            const bool odd = ((yy / 6) & 1) != 0;
            g.fillRect(fx, yy, 6, 6, odd ? DASH_C_WHITE : DASH_C_DARK);
            g.fillRect(fx + 6, yy, 6, 6, odd ? DASH_C_DARK : DASH_C_WHITE);
        }
    }

    if (s.cp_valid) {                                        // practice: checkpoint diamond
        const int sx = s.cp_col * DASH_T - ox + DASH_T / 2;
        const int sy = GY - 20;
        if (sx > -10 && sx < W + 10) {
            g.fillTriangle(sx, sy - 8, sx + 6, sy, sx, sy + 8, 0x3AFF6B);
            g.fillTriangle(sx, sy - 8, sx - 6, sy, sx, sy + 8, 0x1FB84A);
            g.fillRect(sx - 1, sy - 1, 3, 3, DASH_C_WHITE);
        }
    }
    if (!s.dead) {
        const float cx = (float)PX + DASH_P / 2.0f;
        const float cy = s.py + DASH_P / 2.0f;
        const float rad = s.ang * 0.0174533f;
        dash_rot_sq(g, cx, cy, 6.5f, rad, DASH_C_DARK);
        dash_rot_sq(g, cx, cy, 5.5f, rad, DASH_C_BODY);
        dash_rot_sq(g, cx, cy, 2.2f, rad, DASH_C_EYE);
    } else {
        for (int i = 0; i < DASH_PARTS; i++)
            g.fillRect((int)s.qx[i], (int)s.qy[i], 4, 4, (i & 1) ? DASH_C_BODY : DASH_C_ORANGE);
    }

    g.fillRect(4, 2, W - 8, 4, DASH_C_BAR);                  // HUD
    const int bw = (W - 8) * s.pct / 100;
    if (bw > 0) g.fillRect(4, 2, bw, 4, L.col);
    char buf[40];
    g.setTextSize(1.0f);
    g.setTextColor(DASH_C_WHITE);
    snprintf(buf, sizeof(buf), "%d%%", s.pct);
    g.drawString(buf, 4, 9);
    snprintf(buf, sizeof(buf), "Try %d", s.attempt);
    g.drawString(buf, W - g.textWidth(buf) - 4, 9);
    if (s.practice) dash_ctext(g, 0, W, "PRACTICE", 9, 1, 0x3AFF6B);
}

// ---------------- menus ----------------
static void dash_pause_draw(int W, int H, int sel, uint32_t accent, bool practice) {
    char pr[24];
    snprintf(pr, sizeof(pr), "Practice: %s", practice ? "ON" : "OFF");
    const char *items[4] = { "Continue", pr, "Level select", "Quit game" };
    lgfx::LGFXBase &g = M5.Display;
    int bw = W - 40;
    if (bw > 200) bw = 200;
    if (bw < 100) bw = W - 10;
    const int bh = 112;
    const int x = (W - bw) / 2, y = (H - bh) / 2;
    g.fillRect(x, y, bw, bh, DASH_C_PANEL);
    g.drawRect(x, y, bw, bh, accent);
    dash_ctext(g, x, bw, "PAUSED", y + 4, 1, accent);
    for (int i = 0; i < 4; i++) {
        const int ry = y + 16 + i * 23;
        const bool on = (i == sel);
        if (on) g.fillRect(x + 6, ry, bw - 12, 21, accent);
        dash_ctext(g, x + 6, bw - 12, items[i], ry + 3, 2, on ? DASH_C_DARK : DASH_C_WHITE);
    }
}

// 0 = continue, 1 = practice on/off (restarts the level), 2 = level select, 3 = quit
static int dash_pause(int W, int H, uint32_t accent, bool practice) {
    int sel = 0;
    dash_pause_draw(W, H, sel, accent, practice);
    while (true) {
        k85_input_update();
        if (k85_btn_a_pressed() && !k85_btn_b_is_down()) {
            k85_wake_screen();
            sel = (sel + 1) % 4;
            dash_pause_draw(W, H, sel, accent, practice);
        }
        if (k85_btn_b_pressed() && !k85_btn_a_is_down()) {
            k85_wake_screen();
            return sel;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void dash_select_draw(int sel) {
    lgfx::LGFXBase &g = M5.Display;
    const int W = M5.Display.width(), H = M5.Display.height();
    const DashLevel &L = k_levels[sel];
    const uint32_t bg = k85_get_bg(), fg = k85_get_fg(), ac = k85_get_accent();
    g.fillScreen(bg);
    g.setTextSize(1.0f);
    g.setTextColor(ac);
    g.drawString("DASH", 6, 4);
    for (int i = 0; i < DASH_LEVELS; i++) {                  // level dots
        const int dx = W - 10 - (DASH_LEVELS - 1 - i) * 12;
        if (i == sel) g.fillCircle(dx, 8, 4, k_levels[i].col);
        else g.drawCircle(dx, 8, 4, k_levels[i].col);
    }
    const int cy0 = 18, ch = H - 18 - 16;
    g.fillRect(6, cy0, W - 12, ch, L.sky);
    g.drawRect(6, cy0, W - 12, ch, L.col);
    char buf[48];
    snprintf(buf, sizeof(buf), "%d. %s", sel + 1, L.name);
    dash_ctext(g, 6, W - 12, buf, cy0 + 6, 2, DASH_C_WHITE);
    dash_ctext(g, 6, W - 12, L.diff, cy0 + 28, 2, L.col);
    const int bw = 16, gap = 4, total = DASH_LEVELS * bw + (DASH_LEVELS - 1) * gap;
    const int bx = (W - total) / 2, by = cy0 + 52;           // difficulty blocks
    for (int i = 0; i < DASH_LEVELS; i++) {
        const int x = bx + i * (bw + gap);
        if (i < L.stars) g.fillRect(x, by, bw, 9, L.col);
        else g.drawRect(x, by, bw, 9, L.col);
    }
    int best = k85_get_high_score(L.key);
    if (best < 0) best = 0;
    if (best > 100) best = 100;
    snprintf(buf, sizeof(buf), "Best: %d%%%s", best, best >= 100 ? "  CLEARED" : "");
    dash_ctext(g, 6, W - 12, buf, cy0 + 68, 1, DASH_C_WHITE);
    g.fillRect(16, cy0 + 82, W - 32, 6, DASH_C_BAR);
    if (best > 0) g.fillRect(16, cy0 + 82, (W - 32) * best / 100, 6, L.col);
    dash_ctext(g, 6, W - 12, "hold B = practice", cy0 + 91, 1, 0x7FE8A0);
    g.setTextSize(1.0f);
    g.setTextColor(fg);
    g.drawString("A=next B=play A+B=back", 6, H - 11);
}

// returns the chosen level, -1 = back to the Games menu
static int dash_select(int sel) {
    dash_select_draw(sel);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return -1; }
        if (k85_btn_a_pressed() && !k85_btn_b_is_down()) {
            k85_wake_screen();
            sel = (sel + 1) % DASH_LEVELS;
            dash_select_draw(sel);
        }
        if (k85_btn_b_pressed() && !k85_btn_a_is_down()) {
            k85_wake_screen();
            const uint32_t t_b = dash_now_ms();             // B held ~0.5 s = practice mode
            while (k85_btn_b_is_down() && dash_now_ms() - t_b < 500) {
                vTaskDelay(pdMS_TO_TICKS(20));
                k85_input_update();
            }
            s_practice = k85_btn_b_is_down();
            return sel;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void dash_complete_screen(const DashLevel &L, int attempts, bool practice) {
    lgfx::LGFXBase &g = M5.Display;
    const int W = M5.Display.width(), H = M5.Display.height();
    g.fillScreen(L.sky);
    dash_ctext(g, 0, W, "LEVEL", H / 2 - 40, 2, L.col);
    dash_ctext(g, 0, W, "COMPLETE!", H / 2 - 22, 2, L.col);
    char buf[48];
    if (practice) snprintf(buf, sizeof(buf), "%s - practice (not saved)", L.name);
    else snprintf(buf, sizeof(buf), "%s - %d tries", L.name, attempts);
    dash_ctext(g, 0, W, buf, H / 2 + 6, 1, DASH_C_WHITE);
    dash_ctext(g, 0, W, "B = continue", H / 2 + 24, 1, DASH_C_WHITE);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_b_pressed() && !k85_btn_a_is_down()) return;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------------- the game ----------------
// returns 0 = back to the level list, 1 = leave the game, 2 = level completed
static int dash_play(int lv) {
    const DashLevel &L = k_levels[lv];
    dash_build(L.spec);
    s_rng ^= (uint32_t)esp_timer_get_time();

    const int W = M5.Display.width();
    const int H = M5.Display.height();
    const int GY = (H >= 120) ? (H - 30) : (H * 3 / 4);
    const int PX = W / 5;
    const int FIN = (s_cols - 5) * DASH_T;                   // world x of the finish line
    const float span = (float)(FIN - PX);

    M5Canvas canvas(&M5.Display);                            // off-screen frame (PSRAM): no flicker
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    const bool use_canvas = (canvas.createSprite(W, H) != nullptr);
    lgfx::LGFXBase &g = use_canvas ? (lgfx::LGFXBase &)canvas : (lgfx::LGFXBase &)M5.Display;

    DashState s;
    memset(&s, 0, sizeof(s));
    s.attempt = 1;
    s.practice = s_practice;
    dash_reset(s, GY);
    int best = 0;
    int result = 0;

    while (true) {
        const uint32_t t0 = dash_now_ms();
        k85_input_update();
        if (k85_btn_a_pressed()) { k85_wake_screen(); s.buf = 4; }
        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            const int ch = dash_pause(W, H, L.col, s.practice);
            if (ch == 2) { result = 0; break; }
            if (ch == 3) { result = 1; break; }
            if (ch == 1) {                                   // practice on/off: restart the level
                s.practice = !s.practice; s.cp_valid = false; s.attempt = 1; best = 0;
                dash_respawn(s, GY, PX);
            }
            s.buf = 0;
            k85_input_update();
            vTaskDelay(pdMS_TO_TICKS(400));
            continue;
        }

        if (s.dead) {
            s.dead_f++;
            for (int i = 0; i < DASH_PARTS; i++) {
                s.qx[i] += s.qvx[i]; s.qy[i] += s.qvy[i]; s.qvy[i] += 0.45f;
            }
            if (s.dead_f > 22) { s.attempt++; dash_respawn(s, GY, PX); }
        } else {
            const bool a_down = k85_btn_a_is_down();
            if (s.on_ground && (a_down || s.buf > 0)) { s.vy = -DASH_JUMP_V; s.on_ground = false; s.buf = 0; }
            if (s.buf > 0) s.buf--;

            const float prev_bottom = s.py + DASH_P;
            s.vy += DASH_GRAV;
            if (s.vy > DASH_VMAX) s.vy = DASH_VMAX;
            s.py += s.vy;
            s.wx += L.speed;
            s.on_ground = false;

            const float pwx = s.wx + (float)PX;              // cube world x (left edge)
            const int c0 = (int)floorf((pwx + 1.0f) / DASH_T);
            const int c1 = (int)floorf((pwx + DASH_P - 2.0f) / DASH_T);
            bool crash = false;
            for (int c = c0; c <= c1 && !crash; c++) {
                for (int r = 0; r < 3; r++) {
                    const uint8_t t = dash_cell(r, c);
                    if (t == 0 || t == 3) continue;
                    const float ty = (float)(GY - (r + 1) * DASH_T);
                    const float top = s.py, bot = s.py + DASH_P;
                    if (t == 1) {
                        if (bot > ty && top < ty + DASH_T) {
                            if (prev_bottom <= ty + 3.0f && s.vy >= 0.0f) {     // came from above: land on it
                                s.py = ty - DASH_P; s.vy = 0.0f; s.on_ground = true;
                            } else {
                                crash = true;                                    // hit the side
                                break;
                            }
                        }
                    } else {
                        const float sx0 = (float)(c * DASH_T) + 3.0f;
                        const float sx1 = (float)(c * DASH_T) + 9.0f;
                        if (pwx + 2.0f < sx1 && pwx + DASH_P - 2.0f > sx0 &&
                            top + 2.0f < ty + DASH_T && bot - 1.0f > ty + 4.0f) {
                            crash = true;                                        // spike
                            break;
                        }
                    }
                }
            }
            if (!crash && !s.on_ground && s.vy >= 0.0f && s.py + DASH_P >= (float)GY && prev_bottom <= (float)GY + 1.0f) {
                bool sup = false;
                const int ga = (int)floorf((pwx + 2.0f) / DASH_T);
                const int gb = (int)floorf((pwx + DASH_P - 3.0f) / DASH_T);
                for (int c = ga; c <= gb; c++) if (dash_cell(0, c) != 3) sup = true;
                if (sup) { s.py = (float)GY - DASH_P; s.vy = 0.0f; s.on_ground = true; }
            }
            if (s.py > (float)GY + 14.0f) crash = true;                          // fell into a pit

            if (s.on_ground) s.ang = roundf(s.ang / 90.0f) * 90.0f;
            else s.ang += 90.0f / 19.0f;
            if (s.ang >= 360.0f) s.ang -= 360.0f;

            int pct = (int)(s.wx * 100.0f / span);
            if (pct > 100) pct = 100;
            if (pct < 0) pct = 0;
            s.pct = pct;

            if (s.practice && !crash) {                          // practice: a diamond every ~3 s on the first safe column
                s.cp_timer++;
                if (s.cp_timer >= DASH_CP_FRAMES) {
                    const int cc = (int)floorf((pwx + DASH_P * 0.5f) / DASH_T);
                    if (cc > s.cp_col && dash_cp_safe(cc)) { s.cp_valid = true; s.cp_col = cc; s.cp_timer = 0; }
                }
            }
            if (crash) {
                s.dead = true; s.dead_f = 0;
                dash_burst(s, (float)PX + DASH_P / 2.0f, s.py + DASH_P / 2.0f);
                if (!s.practice && s.pct > best) { best = s.pct; k85_set_high_score_if_better(L.key, s.pct); }
            } else if (pwx >= (float)FIN) {
                s.pct = 100;
                if (!s.practice) k85_set_high_score_if_better(L.key, 100);
                result = 2;
                break;
            }
        }

        dash_draw(g, W, H, GY, PX, FIN, L, s);
        if (use_canvas) canvas.pushSprite(0, 0);

        const int el = (int)(dash_now_ms() - t0);
        const int rem = DASH_FRAME_MS - el;
        vTaskDelay(rem >= 10 ? (TickType_t)pdMS_TO_TICKS(rem) : (TickType_t)1);
    }

    if (use_canvas) canvas.deleteSprite();
    if (result == 2) dash_complete_screen(L, s.attempt, s.practice);
    return result;
}

void k85_run_dash(void) {
    int sel = 0;
    while (true) {
        sel = dash_select(sel);
        if (sel < 0) return;
        const int r = dash_play(sel);
        if (r == 1) return;
        if (r == 2 && sel < DASH_LEVELS - 1) sel++;
    }
}