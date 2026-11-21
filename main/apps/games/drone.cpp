// K85DRONE1: Drone game (Games -> Drone).
// With a display: hold A = up, hold B = down, double A = menu (same as the gear), double B = boost.
// No-display web mode: the page shows a stick (or arrows) + BOOST; tap the gear in the top-left corner = menu.
#include "drone.h"
#include "input.h"
#include "power.h"
#include "sound.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

#define DRONE_FILE "/littlefs/drone.dat"
#define DRONE_MAGIC 0x4B38444Eu
#define D_OBS_MAX 10
#define D_COIN_MAX 28
#define D_CLOUDS 5

struct DSet { uint8_t diff, lives, motor, fx, sky, boost; };
struct DFile {
    uint32_t magic;
    DSet s;
    uint32_t best, last, best_coins;
    uint32_t sv_ok, sv_dist, sv_coins, sv_lives;
};
static DFile g_d;

struct DObs { bool on; uint8_t type; float x, y, y0, w, h, ph, k; };
struct DCoin { bool on; float x, y; };
struct DCloud { float x, y; uint8_t s; };
struct DGame {
    float x, y, vx, vy, t, spd, dist;
    int coins, collected, bonus, lives;
    float boost_t, inv_t, spawn_t, coin_t, msg_t, ready_t;
    int state;                 // 1 = play, 2 = game over
    bool newrec;
    char msg[24];
    DObs obs[D_OBS_MAX];
    DCoin coin[D_COIN_MAX];
    DCloud cl[D_CLOUDS];
};
static DGame gm;

static const float D_SPEED[3] = { 60.0f, 78.0f, 96.0f };
static const float D_SPAWN[3] = { 1.35f, 1.05f, 0.80f };
static const float D_FORGIVE[3] = { 3.0f, 2.0f, 1.0f };
static const int D_BOOST_COST[3] = { 4, 8, 12 };

struct JoyWant {
    JoyWant() { k85_joy_want(true); }
    ~JoyWant() { k85_joy_want(false); }
};

static float frand(void) { return (float)(esp_random() & 0xFFFF) / 65535.0f; }
static int irand(int a, int b) {
    if (b <= a) return a;
    return a + (int)(esp_random() % (uint32_t)(b - a + 1));
}
static int64_t now_us(void) { return esp_timer_get_time(); }
static int score_now(void) { return (int)gm.dist + gm.collected * 10 + gm.bonus; }

// ---------- settings / records file ----------
static void d_defaults(void) {
    memset(&g_d, 0, sizeof(g_d));
    g_d.magic = DRONE_MAGIC;
    g_d.s.diff = 1;
    g_d.s.lives = 3;
    g_d.s.motor = 1;
    g_d.s.fx = 1;
    g_d.s.sky = 0;
    g_d.s.boost = 1;
}

static void d_load(void) {
    d_defaults();
    FILE *f = fopen(DRONE_FILE, "rb");
    if (!f) f = fopen(DRONE_FILE ".tmp", "rb");
    if (!f) return;
    DFile t;
    size_t n = fread(&t, 1, sizeof(t), f);
    fclose(f);
    if (n == sizeof(t) && t.magic == DRONE_MAGIC && t.s.diff < 3 && t.s.sky < 3 && t.s.boost < 3 &&
        (t.s.lives == 1 || t.s.lives == 3 || t.s.lives == 5)) {
        g_d = t;
    }
}

static bool d_save(void) {
    FILE *f = fopen(DRONE_FILE ".tmp", "wb");
    if (!f) return false;
    bool ok = (fwrite(&g_d, 1, sizeof(g_d), f) == sizeof(g_d));
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(DRONE_FILE ".tmp");
        return false;
    }
    remove(DRONE_FILE);
    return rename(DRONE_FILE ".tmp", DRONE_FILE) == 0;
}

// ---------- menus (drawn straight on the display) ----------
static void d_msg(const char *s) {
    const int W = M5.Display.width(), H = M5.Display.height();
    M5.Display.fillRect(0, H / 2 - 14, W, 28, 0x000000);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(0xFFFFFF, 0x000000);
    M5.Display.setCursor(8, H / 2 - 8);
    M5.Display.print(s);
    vTaskDelay(pdMS_TO_TICKS(800));
}

// A = next, B = choose, A+B hold = -1
static int d_pick(const char *title, const char *const items[], int n, int sel) {
    const int W = M5.Display.width(), H = M5.Display.height();
    const int ly0 = 26, lh = 20;
    int vis = (H - ly0) / lh;
    if (vis < 1) vis = 1;
    int top = 0;
    auto draw = [&]() {
        if (sel < top) top = sel;
        else if (sel >= top + vis) top = sel - vis + 1;
        M5.Display.fillScreen(0x101828);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0x66CCFF, 0x101828);
        M5.Display.setCursor(6, 8);
        M5.Display.print(title);
        M5.Display.setTextSize(2);
        int end = top + vis;
        if (end > n) end = n;
        for (int i = top; i < end; i++) {
            const int y = ly0 + (i - top) * lh;
            const bool s = (i == sel);
            if (s) M5.Display.fillRect(0, y - 1, W, lh, 0x66CCFF);
            M5.Display.setTextColor(s ? 0x000000 : 0xFFFFFF, s ? 0x66CCFF : 0x101828);
            M5.Display.setCursor(6, y + 1);
            M5.Display.print(items[i]);
        }
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xAAAAAA, 0x101828);
        if (top > 0) {
            M5.Display.setCursor(W - 10, ly0);
            M5.Display.print("^");
        }
        if (end < n) {
            M5.Display.setCursor(W - 10, H - 10);
            M5.Display.print("v");
        }
    };
    draw();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return -1; }
        if (k85_btn_a_pressed()) { k85_wake_screen(); sel = (sel + 1) % n; draw(); }
        if (k85_btn_b_pressed()) { k85_wake_screen(); return sel; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void d_best(void) {
    const int H = M5.Display.height();
    M5.Display.fillScreen(0x101828);
    M5.Display.setTextColor(0xFFFFFF, 0x101828);
    M5.Display.setTextSize(2);
    char b[40];
    snprintf(b, sizeof(b), "Best: %u", (unsigned)g_d.best);
    M5.Display.setCursor(8, 14);
    M5.Display.print(b);
    snprintf(b, sizeof(b), "Last: %u", (unsigned)g_d.last);
    M5.Display.setCursor(8, 40);
    M5.Display.print(b);
    snprintf(b, sizeof(b), "Coins: %u", (unsigned)g_d.best_coins);
    M5.Display.setCursor(8, 66);
    M5.Display.print(b);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x9CCBFF, 0x101828);
    M5.Display.setCursor(8, H - 14);
    M5.Display.print("A or B = back");
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed() || k85_btn_b_pressed()) { k85_wake_screen(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void d_settings(void) {
    static const char *dn[] = { "Easy", "Normal", "Hard" };
    static const char *sn[] = { "Day", "Dusk", "Night" };
    int sel = 0;
    while (true) {
        char l[8][24];
        const char *it[8];
        snprintf(l[0], sizeof(l[0]), "Level: %s", dn[g_d.s.diff]);
        snprintf(l[1], sizeof(l[1]), "Lives: %d", (int)g_d.s.lives);
        snprintf(l[2], sizeof(l[2]), "Motor: %s", g_d.s.motor ? "on" : "off");
        snprintf(l[3], sizeof(l[3]), "Effects: %s", g_d.s.fx ? "on" : "off");
        snprintf(l[4], sizeof(l[4]), "Sky: %s", sn[g_d.s.sky]);
        snprintf(l[5], sizeof(l[5]), "Boost: %d coins", D_BOOST_COST[g_d.s.boost]);
        snprintf(l[6], sizeof(l[6]), "Reset record");
        snprintf(l[7], sizeof(l[7]), "Back");
        for (int i = 0; i < 8; i++) it[i] = l[i];
        int r = d_pick("DRONE settings", it, 8, sel);
        if (r < 0 || r == 7) break;
        sel = r;
        if (r == 0) g_d.s.diff = (uint8_t)((g_d.s.diff + 1) % 3);
        else if (r == 1) g_d.s.lives = (g_d.s.lives == 1) ? 3 : ((g_d.s.lives == 3) ? 5 : 1);
        else if (r == 2) g_d.s.motor = g_d.s.motor ? 0 : 1;
        else if (r == 3) g_d.s.fx = g_d.s.fx ? 0 : 1;
        else if (r == 4) g_d.s.sky = (uint8_t)((g_d.s.sky + 1) % 3);
        else if (r == 5) g_d.s.boost = (uint8_t)((g_d.s.boost + 1) % 3);
        else if (r == 6) {
            const char *yn[] = { "No", "Yes, reset" };
            if (d_pick("Reset record?", yn, 2, 0) == 1) {
                g_d.best = 0; g_d.last = 0; g_d.best_coins = 0;
            }
        }
    }
    if (!d_save()) d_msg("Save failed");
}

enum { A_CONT, A_NEW, A_SAVE, A_LOAD, A_SET, A_BEST, A_EXIT };

static int d_menu(bool playing) {
    int sel = 0;
    while (true) {
        const char *it[7];
        int id[7];
        int n = 0;
        if (playing) { it[n] = "Continue"; id[n++] = A_CONT; }
        it[n] = "New game"; id[n++] = A_NEW;
        if (playing) { it[n] = "Save game"; id[n++] = A_SAVE; }
        if (g_d.sv_ok) { it[n] = "Load game"; id[n++] = A_LOAD; }
        it[n] = "Settings"; id[n++] = A_SET;
        it[n] = "Best / last"; id[n++] = A_BEST;
        it[n] = "Exit game"; id[n++] = A_EXIT;
        if (sel >= n) sel = n - 1;
        int r = d_pick("DRONE", it, n, sel);
        if (r < 0) return playing ? A_CONT : A_EXIT;
        sel = r;
        int a = id[r];
        if (a == A_SAVE) {
            g_d.sv_ok = 1;
            g_d.sv_dist = (uint32_t)gm.dist;
            g_d.sv_coins = (uint32_t)gm.coins;
            g_d.sv_lives = (uint32_t)gm.lives;
            d_msg(d_save() ? "Saved" : "Save failed");
            continue;
        }
        if (a == A_SET) { d_settings(); continue; }
        if (a == A_BEST) { d_best(); continue; }
        return a;
    }
}

// ---------- game logic ----------
static void g_reset(int W, int H) {
    memset(&gm, 0, sizeof(gm));
    gm.x = W * 0.25f;
    gm.y = H * 0.5f;
    gm.lives = g_d.s.lives;
    gm.state = 1;
    gm.ready_t = 2.0f;
    gm.spawn_t = 1.2f;
    gm.coin_t = 0.6f;
    for (int i = 0; i < D_CLOUDS; i++) {
        gm.cl[i].x = frand() * W;
        gm.cl[i].y = 12.0f + frand() * (H * 0.45f);
        gm.cl[i].s = (uint8_t)irand(0, 2);
    }
}

static void spawn_obs(int W, int H) {
    for (int i = 0; i < D_OBS_MAX; i++) {
        if (gm.obs[i].on) continue;
        DObs &o = gm.obs[i];
        memset(&o, 0, sizeof(o));
        o.on = true;
        const int r = irand(0, 99);
        o.type = (uint8_t)(r < 35 ? 0 : (r < 60 ? 1 : (r < 80 ? 2 : 3)));
        o.x = (float)(W + 10);
        o.ph = frand() * 6.28f;
        if (o.type == 0) {            // plane: long and fast
            o.w = 30.0f; o.h = 11.0f; o.y = (float)irand(12, H - 28); o.k = 1.9f;
        } else if (o.type == 1) {     // bird: sine path
            o.w = 12.0f; o.h = 7.0f; o.y = (float)irand(16, H - 32); o.k = 1.35f;
        } else if (o.type == 2) {     // balloon: slow, big
            o.w = 16.0f; o.h = 22.0f; o.y = (float)irand(16, H - 48); o.k = 1.0f;
        } else {                      // pylon from the ground
            o.w = 8.0f; o.h = (float)irand(H / 4, H * 6 / 10); o.y = (float)H - o.h; o.k = 1.0f;
        }
        o.y0 = o.y;
        return;
    }
}

static void spawn_coins(int W, int H) {
    const int pat = irand(0, 2);
    const float base = (float)irand(24, H - 24);
    for (int k = 0; k < 5; k++) {
        for (int i = 0; i < D_COIN_MAX; i++) {
            if (gm.coin[i].on) continue;
            float y = base;
            if (pat == 1) y = base + sinf((float)k * 0.9f) * 14.0f;
            if (pat == 2) y = base + (float)(k - 2) * 7.0f;
            if (y < 14.0f) y = 14.0f;
            if (y > (float)(H - 14)) y = (float)(H - 14);
            gm.coin[i].on = true;
            gm.coin[i].x = (float)(W + 8 + k * 14);
            gm.coin[i].y = y;
            break;
        }
    }
}

static bool obs_hit(const DObs &o, float px, float py, float fg) {
    const float dx0 = px - 8.0f, dx1 = px + 8.0f, dy0 = py - 4.0f, dy1 = py + 4.0f;
    const float ox0 = o.x + fg, ox1 = o.x + o.w - fg, oy0 = o.y + fg, oy1 = o.y + o.h - fg;
    return dx1 > ox0 && dx0 < ox1 && dy1 > oy0 && dy0 < oy1;
}

static void g_finish(void) {
    const uint32_t sc = (uint32_t)score_now();
    gm.newrec = sc > g_d.best;
    if (gm.newrec) g_d.best = sc;
    g_d.last = sc;
    if ((uint32_t)gm.collected > g_d.best_coins) g_d.best_coins = (uint32_t)gm.collected;
    d_save();
    if (g_d.s.fx) k85_play_tone(330, 150);
}

static void g_step(float dt, int W, int H, float tx, float ty, bool boost_req) {
    const int diff = g_d.s.diff;
    gm.t += dt;

    if (boost_req && gm.boost_t <= 0.0f) {
        const int cost = D_BOOST_COST[g_d.s.boost];
        if (gm.coins >= cost) {
            gm.coins -= cost;
            gm.boost_t = 3.0f;
            if (g_d.s.fx) k85_play_tone(990, 120);
        } else {
            snprintf(gm.msg, sizeof(gm.msg), "Need %d coins", cost);
            gm.msg_t = 1.2f;
            if (g_d.s.fx) k85_play_tone(140, 120);
        }
    }

    const float k = 1.0f - expf(-6.0f * dt);
    gm.vx += (tx - gm.vx) * k;
    gm.vy += (ty - gm.vy) * k;
    gm.x += gm.vx * dt;
    gm.y += gm.vy * dt;
    if (gm.x < 12.0f) gm.x = 12.0f;
    if (gm.x > W * 0.65f) gm.x = W * 0.65f;
    if (gm.y < 12.0f) gm.y = 12.0f;
    if (gm.y > (float)(H - 14)) gm.y = (float)(H - 14);

    const float base = D_SPEED[diff] * (1.0f + fminf(gm.dist / 1500.0f, 0.8f));
    gm.spd = base * (gm.boost_t > 0.0f ? 1.9f : 1.0f);
    gm.dist += gm.spd * dt / 10.0f;

    if (gm.boost_t > 0.0f) {
        gm.boost_t -= dt;
        if (gm.boost_t <= 0.0f) { gm.boost_t = 0.0f; gm.inv_t = 0.8f; }
    }
    if (gm.inv_t > 0.0f) gm.inv_t -= dt;
    if (gm.msg_t > 0.0f) gm.msg_t -= dt;

    for (int i = 0; i < D_CLOUDS; i++) {
        gm.cl[i].x -= gm.spd * dt * (0.2f + 0.1f * (float)gm.cl[i].s);
        if (gm.cl[i].x < -30.0f) {
            gm.cl[i].x = (float)(W + 20);
            gm.cl[i].y = 12.0f + frand() * (H * 0.45f);
        }
    }

    gm.spawn_t -= dt;
    if (gm.spawn_t <= 0.0f) {
        spawn_obs(W, H);
        gm.spawn_t = D_SPAWN[diff] * (0.7f + 0.6f * frand()) / (1.0f + fminf(gm.dist / 2500.0f, 0.5f));
    }
    gm.coin_t -= dt;
    if (gm.coin_t <= 0.0f) {
        spawn_coins(W, H);
        gm.coin_t = 1.6f + frand() * 1.4f;
    }

    const float fg = D_FORGIVE[diff];
    for (int i = 0; i < D_OBS_MAX; i++) {
        DObs &o = gm.obs[i];
        if (!o.on) continue;
        o.x -= gm.spd * o.k * dt;
        if (o.type == 1) o.y = o.y0 + sinf(gm.t * 4.0f + o.ph) * 10.0f;
        else if (o.type == 2) o.y = o.y0 + sinf(gm.t * 1.2f + o.ph) * 14.0f;
        if (o.x + o.w < -6.0f) { o.on = false; continue; }
        if (!obs_hit(o, gm.x, gm.y, fg)) continue;
        if (gm.boost_t > 0.0f) {
            o.on = false;
            gm.bonus += 5;
            if (g_d.s.fx) k85_play_tone(660, 30);
        } else if (gm.inv_t <= 0.0f) {
            gm.lives--;
            gm.inv_t = 1.6f;
            gm.vy = -30.0f;
            if (g_d.s.fx) k85_play_tone(110, 200);
            if (gm.lives <= 0) { gm.state = 2; return; }
        }
    }

    const float rr = (gm.boost_t > 0.0f) ? 400.0f : 121.0f;
    for (int i = 0; i < D_COIN_MAX; i++) {
        DCoin &c = gm.coin[i];
        if (!c.on) continue;
        c.x -= gm.spd * dt;
        if (c.x < -8.0f) { c.on = false; continue; }
        const float dx = c.x - gm.x, dy = c.y - gm.y;
        if (dx * dx + dy * dy < rr) {
            c.on = false;
            gm.coins++;
            gm.collected++;
            if (g_d.s.fx) k85_play_tone(1760, 30);
        }
    }
}

// ---------- drawing (into a canvas in PSRAM, then one push) ----------
static uint32_t mix(uint32_t a, uint32_t b, int t, int n) {
    const int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    const int br = (b >> 16) & 255, bgc = (b >> 8) & 255, bb = b & 255;
    const int r = ar + (br - ar) * t / n;
    const int g = ag + (bgc - ag) * t / n;
    const int bl = ab + (bb - ab) * t / n;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static void hud_text(M5Canvas &cv, int x, int y, const char *s, uint32_t col) {
    cv.setTextColor(0x000000);
    cv.setCursor(x + 1, y + 1);
    cv.print(s);
    cv.setTextColor(col);
    cv.setCursor(x, y);
    cv.print(s);
}

static void draw_gear(M5Canvas &cv, int cx, int cy) {
    cv.fillCircle(cx, cy, 5, 0xFFFFFF);
    for (int a = 0; a < 360; a += 45) {
        const float r = (float)a * 0.0174533f;
        cv.drawLine(cx + (int)(cosf(r) * 4.0f), cy + (int)(sinf(r) * 4.0f),
                    cx + (int)(cosf(r) * 8.0f), cy + (int)(sinf(r) * 8.0f), 0xFFFFFF);
    }
    cv.fillCircle(cx, cy, 2, 0x303030);
}

static void draw_heart(M5Canvas &cv, int x, int y) {
    cv.fillCircle(x - 2, y, 2, 0xFF3355);
    cv.fillCircle(x + 2, y, 2, 0xFF3355);
    cv.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 6, 0xFF3355);
}

static void draw_drone(M5Canvas &cv, int x, int y, int frame, bool boost) {
    const uint32_t body = boost ? 0x00E5FF : 0xE0E0E0;
    if (boost) cv.fillTriangle(x - 8, y, x - 24 - (frame % 3) * 3, y + 1, x - 8, y + 4, 0xFF9800);
    cv.fillRoundRect(x - 7, y - 2, 14, 6, 2, body);
    cv.fillRect(x - 3, y + 4, 6, 2, 0x404040);
    cv.fillCircle(x + 6, y + 2, 2, 0x2196F3);
    cv.drawLine(x - 6, y - 2, x - 11, y - 6, 0x606060);
    cv.drawLine(x + 6, y - 2, x + 11, y - 6, 0x606060);
    const int rl = 4 + ((frame & 1) ? 2 : 0);
    cv.drawFastHLine(x - 11 - rl, y - 7, rl * 2, 0x202020);
    cv.drawFastHLine(x + 11 - rl, y - 7, rl * 2, 0x202020);
    cv.drawFastHLine(x - 10 - rl, y - 8, rl * 2 - 2, 0x909090);
    cv.drawFastHLine(x + 12 - rl, y - 8, rl * 2 - 2, 0x909090);
    if (boost) cv.drawCircle(x, y, 13, 0x66FFFF);
}

static void draw_obs(M5Canvas &cv, const DObs &o, int frame) {
    const int x = (int)o.x, y = (int)o.y;
    switch (o.type) {
        case 0: {
            cv.fillRoundRect(x, y + 3, 30, 6, 3, 0xECEFF1);
            cv.fillTriangle(x, y + 6, x + 7, y + 3, x + 7, y + 9, 0xE53935);
            cv.fillTriangle(x + 24, y + 3, x + 30, y + 3, x + 30, y - 2, 0xE53935);
            cv.fillTriangle(x + 11, y + 6, x + 21, y + 6, x + 16, y + 11, 0x90A4AE);
            cv.fillTriangle(x + 11, y + 6, x + 21, y + 6, x + 16, y + 1, 0xB0BEC5);
            for (int i = 0; i < 4; i++) cv.fillRect(x + 9 + i * 4, y + 4, 2, 2, 0x455A64);
            break;
        }
        case 1: {
            const bool up = sinf(gm.t * 18.0f + o.ph) > 0.0f;
            cv.fillCircle(x + 6, y + 4, 3, 0x37474F);
            cv.fillTriangle(x, y + 4, x + 3, y + 3, x + 3, y + 5, 0xFFA000);
            cv.drawLine(x + 6, y + 3, x + 9, up ? y - 1 : y + 6, 0x37474F);
            cv.drawLine(x + 7, y + 3, x + 11, up ? y : y + 6, 0x37474F);
            break;
        }
        case 2: {
            const uint32_t bc = (((int)(o.ph * 10.0f)) & 1) ? 0xE53935 : 0x43A047;
            cv.fillCircle(x + 8, y + 8, 8, bc);
            cv.drawLine(x + 8, y, x + 8, y + 16, 0xFFFFFF);
            cv.drawLine(x + 3, y + 14, x + 6, y + 19, 0x5D4037);
            cv.drawLine(x + 13, y + 14, x + 10, y + 19, 0x5D4037);
            cv.fillRect(x + 5, y + 19, 6, 3, 0x8D6E63);
            break;
        }
        default: {
            const int h = (int)o.h;
            cv.fillRect(x, y, 8, h, 0x555B66);
            for (int yy = y + 6; yy < y + h; yy += 12) {
                cv.drawLine(x, yy, x + 7, yy + 6, 0x8A93A0);
                cv.drawLine(x + 7, yy, x, yy + 6, 0x8A93A0);
            }
            cv.fillCircle(x + 4, y - 2, 2, ((frame >> 3) & 1) ? 0xFF2020 : 0x501010);
            break;
        }
    }
}

static void d_draw(M5Canvas &cv, int W, int H, int frame) {
    static const uint32_t TOP[3] = { 0x4FA8E8, 0x2A1B52, 0x040616 };
    static const uint32_t BOT[3] = { 0xBFE6FF, 0xF08A4B, 0x1B2A5A };
    const int sky = g_d.s.sky;
    const int bands = 8;
    for (int i = 0; i < bands; i++) {
        const int y0 = H * i / bands, y1 = H * (i + 1) / bands;
        cv.fillRect(0, y0, W, y1 - y0 + 1, mix(TOP[sky], BOT[sky], i, bands - 1));
    }
    if (sky == 2) {
        for (int i = 0; i < 16; i++) {
            if ((((frame >> 3) + i) % 5) != 0) cv.drawPixel((i * 53 + 17) % W, (i * 29 + 7) % (H / 2), 0xFFFFFF);
        }
    }
    const uint32_t cc = (sky == 0) ? 0xFFFFFF : ((sky == 1) ? 0xF3C9A8 : 0x39426B);
    for (int i = 0; i < D_CLOUDS; i++) {
        const int cx = (int)gm.cl[i].x, cy = (int)gm.cl[i].y, r = 5 + gm.cl[i].s * 2;
        cv.fillCircle(cx, cy, r, cc);
        cv.fillCircle(cx + r, cy + 2, r - 1, cc);
        cv.fillCircle(cx - r, cy + 2, r - 2, cc);
    }
    cv.fillRect(0, H - 6, W, 6, (sky == 2) ? 0x153A1B : 0x2E7D32);
    const int off = (int)(gm.dist * 10.0f) % 24;
    for (int x = -off; x < W; x += 24) cv.fillRect(x, H - 4, 12, 2, (sky == 2) ? 0x1F5226 : 0x388E3C);

    for (int i = 0; i < D_COIN_MAX; i++) {
        if (!gm.coin[i].on) continue;
        const int cw = 1 + (int)(fabsf(cosf(gm.t * 6.0f + gm.coin[i].y * 0.1f)) * 3.0f);
        cv.fillRoundRect((int)gm.coin[i].x - cw, (int)gm.coin[i].y - 4, cw * 2 + 1, 9, cw, 0xFFC107);
    }
    for (int i = 0; i < D_OBS_MAX; i++) {
        if (gm.obs[i].on) draw_obs(cv, gm.obs[i], frame);
    }
    const bool blink = (gm.inv_t > 0.0f && gm.boost_t <= 0.0f && ((frame >> 1) & 1));
    if (!blink) draw_drone(cv, (int)gm.x, (int)gm.y, frame, gm.boost_t > 0.0f);
    if (gm.inv_t > 1.2f) cv.drawRect(0, 0, W, H, 0xFF0000);

    // HUD
    draw_gear(cv, 11, 11);
    char b[32];
    cv.setTextSize(1);
    snprintf(b, sizeof(b), "SCORE %d", score_now());
    hud_text(cv, 26, 4, b, 0xFFFFFF);
    cv.fillCircle(112, 8, 4, 0xFFC107);
    snprintf(b, sizeof(b), "%d", gm.coins);
    hud_text(cv, 120, 4, b, 0xFFFFFF);
    for (int i = 0; i < gm.lives && i < 5; i++) draw_heart(cv, W - 8 - i * 11, 6);
    const int cost = D_BOOST_COST[g_d.s.boost];
    if (gm.boost_t > 0.0f) {
        cv.fillRect(W - 62, 18, (int)(56.0f * gm.boost_t / 3.0f), 4, 0x00E5FF);
        cv.drawRect(W - 63, 17, 58, 6, 0xFFFFFF);
    } else if (gm.coins >= cost) {
        hud_text(cv, W - 62, 16, "BOOST!", 0x00E5FF);
    }
    if (gm.msg_t > 0.0f) hud_text(cv, 70, 26, gm.msg, 0xFFD54F);

    if (gm.state == 1 && gm.ready_t > 0.0f) {
        cv.fillRoundRect(20, H / 2 - 26, W - 40, 52, 6, 0x101828);
        cv.setTextSize(2);
        cv.setTextColor(0xFFFFFF);
        cv.setCursor(W / 2 - 54, H / 2 - 20);
        cv.print("GET READY");
        cv.setTextSize(1);
        cv.setTextColor(0x9CCBFF);
        cv.setCursor(26, H / 2 + 2);
        cv.print("A up B down 2xA menu 2xB boost");
        cv.setCursor(26, H / 2 + 14);
        cv.print("web: stick + BOOST, gear = menu");
    }
    if (gm.state == 2) {
        cv.fillRoundRect(24, H / 2 - 40, W - 48, 80, 6, 0x101828);
        cv.setTextSize(2);
        cv.setTextColor(0xFF5555);
        cv.setCursor(W / 2 - 54, H / 2 - 34);
        cv.print("GAME OVER");
        cv.setTextSize(1);
        cv.setTextColor(0xFFFFFF);
        snprintf(b, sizeof(b), "Score %d  coins %d", score_now(), gm.collected);
        cv.setCursor(34, H / 2 - 12);
        cv.print(b);
        if (gm.newrec) {
            cv.setTextColor(0xFFD54F);
            cv.setCursor(34, H / 2 - 1);
            cv.print("NEW RECORD!");
        } else {
            snprintf(b, sizeof(b), "Best %u", (unsigned)g_d.best);
            cv.setCursor(34, H / 2 - 1);
            cv.print(b);
        }
        cv.setTextColor(0x9CCBFF);
        cv.setCursor(34, H / 2 + 16);
        cv.print("B retry  A menu  A+B exit");
    }
}

// ---------- entry ----------
void k85_run_drone(void) {
    JoyWant jw;
    d_load();
    const int W = M5.Display.width(), H = M5.Display.height();
    M5Canvas cv(&M5.Display);
    cv.setColorDepth(16);
    cv.setPsram(true);
    if (!cv.createSprite(W, H)) { d_msg("No memory"); return; }
    g_reset(W, H);

    int64_t last = now_us(), last_motor = 0, last_a = 0, last_b = 0;
    bool prev_jb = false;
    int frame = 0;
    while (true) {
        k85_input_update();
        const int64_t now = now_us();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;
        if (dt > 0.06f) dt = 0.06f;
        if (dt < 0.001f) dt = 0.001f;
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }

        int jx = 0, jy = 0, jb = 0, px = 0, py = 0;
        const bool web = k85_joy_get(&jx, &jy, &jb);
        bool pause = false;
        if (k85_pointer_take(&px, &py) && px < 30 && py < 30) pause = true;
        k85_scroll_take();                       // swipes on the page are not used in the game
        const bool a_down = k85_btn_a_is_down(), b_down = k85_btn_b_is_down();
        const bool a_press = k85_btn_a_pressed(), b_press = k85_btn_b_pressed();
        bool boost_req = false;
        if (a_press) {
            k85_wake_screen();
            if (last_a && now - last_a < 350000) { pause = true; last_a = 0; }
            else last_a = now;
        }
        if (b_press) {
            k85_wake_screen();
            if (last_b && now - last_b < 350000) { boost_req = true; last_b = 0; }
            else last_b = now;
        }
        const bool jb_on = web && jb != 0;
        if (jb_on && !prev_jb) boost_req = true;
        prev_jb = jb_on;

        if (gm.state == 2) {
            if (a_press) pause = true;
            if (b_press) { g_reset(W, H); continue; }
        }

        if (pause) {
            k85_speaker_stop();
            const int act = d_menu(gm.state == 1);
            if (act == A_NEW) {
                g_reset(W, H);
            } else if (act == A_LOAD) {
                g_reset(W, H);
                gm.dist = (float)g_d.sv_dist;
                gm.coins = (int)g_d.sv_coins;
                gm.lives = (int)g_d.sv_lives;
                if (gm.lives < 1) gm.lives = 1;
            } else if (act == A_EXIT) {
                break;
            }
            k85_wait_ab_release();
            last = now_us();
            last_a = 0;
            last_b = 0;
            prev_jb = false;
            continue;
        }

        if (gm.state == 1) {
            if (gm.ready_t > 0.0f) {
                gm.ready_t -= dt;
            } else {
                float tx = 0.0f, ty = 0.0f;
                if (web) {
                    tx = (float)jx * 0.7f;
                    ty = (float)jy * 0.85f;
                } else {
                    if (a_down && !b_down) ty = -85.0f;
                    else if (b_down && !a_down) ty = 85.0f;
                    tx = (W * 0.25f - gm.x) * 1.5f;
                }
                g_step(dt, W, H, tx, ty, boost_req);
                if (gm.state == 2) g_finish();
            }
            if (g_d.s.motor && now - last_motor > 55000) {
                last_motor = now;
                const int f = 150 + (int)(fabsf(gm.vy) * 0.8f) + (gm.boost_t > 0.0f ? 70 : 0) + ((frame & 1) ? 14 : 0);
                k85_play_tone((uint32_t)f, 60);
            }
        }

        d_draw(cv, W, H, frame);
        cv.pushSprite(&M5.Display, 0, 0);
        frame++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    k85_speaker_stop();
}