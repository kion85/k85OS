// K85GAMES1: Race: three lanes, smooth steering, traffic with different speeds, overtakes and near misses, engine sound.
// A = lane to the right, B = lane to the left. The best score is kept for the session (the config has no "race" record).
#include "race.h"
#include "game_kit.h"
#include "config.h"
#include "theme.h"

#include <cstdio>
#include <cstring>
#include <cmath>

#define RC_MAX 8
#define RC_TREES 7

static int s_race_best = 0;

struct RcCar { bool on, passed, van; int lane; float y; uint32_t col; };
struct RcTree { float x, y; int r; };

static void rc_draw_car(M5Canvas &cv, int cx, int y, int w, int h, uint32_t body, bool player, bool flame, int frame) {
    const int x = cx - w / 2;
    cv.fillRect(x - 2, y + h / 6, 3, h / 4, 0x15181C);
    cv.fillRect(x + w - 1, y + h / 6, 3, h / 4, 0x15181C);
    cv.fillRect(x - 2, y + h * 5 / 8, 3, h / 4, 0x15181C);
    cv.fillRect(x + w - 1, y + h * 5 / 8, 3, h / 4, 0x15181C);
    cv.fillRoundRect(x, y, w, h, 4, body);
    cv.fillRoundRect(x + 2, y + h / 4, w - 4, h / 2, 3, gk::shade(body, 125));
    cv.fillRect(x + 3, y + 3, w - 6, h / 6, 0x2A3F5F);
    cv.fillRect(x + 3, y + h - h / 6 - 2, w - 6, h / 8, 0x2A3F5F);
    cv.fillRect(x + 1, y + 1, 3, 2, 0xFFF59D);
    cv.fillRect(x + w - 4, y + 1, 3, 2, 0xFFF59D);
    cv.fillRect(x + 1, y + h - 3, 3, 2, player ? 0xFF1744 : 0xB71C1C);
    cv.fillRect(x + w - 4, y + h - 3, 3, 2, player ? 0xFF1744 : 0xB71C1C);
    if (flame) {
        const int fl = 3 + (frame & 3);
        cv.fillTriangle(cx - 3, y + h, cx + 3, y + h, cx, y + h + fl + 3, 0xFF9800);
        cv.fillTriangle(cx - 1, y + h, cx + 1, y + h, cx, y + h + fl, 0xFFEB3B);
    }
}

static bool rc_round(gk::Canvas &c) {
    M5Canvas &cv = c.cv;
    const int W = c.W, H = c.H;
    int lane_w = (W - 24) / 3;
    if (lane_w > 44) lane_w = 44;
    if (lane_w < 24) lane_w = 24;
    const int road_w = lane_w * 3, rx0 = (W - road_w) / 2, rx1 = rx0 + road_w;
    int cw = lane_w - 16;
    if (cw < 14) cw = 14;
    const int ch = cw * 3 / 2 + 8;
    const int py = H - ch - 8;
    const bool scenery = rx0 > 18;
    auto lane_cx = [&](int l) -> float { return (float)(rx0 + l * lane_w + lane_w / 2); };
    static const float LANE_F[3] = { 0.56f, 0.50f, 0.44f };
    static const uint32_t COLS[6] = { 0xE53935, 0xFB8C00, 0x8E24AA, 0xFDD835, 0x00ACC1, 0xEEEEEE };

    int lane = 1, overtakes = 0, near_bonus = 0, frame = 0, last_free = 1;
    float x = lane_cx(1), speed = 95.0f, scroll = 0.0f, dist = 0.0f, spawn_d = 40.0f;
    float ready = 1.4f, flash = 0.0f, motor_t = 0.0f, pop_t = 0.0f, pop_x = 0.0f, crash_t = 0.0f;
    char pop_txt[12] = "";
    bool crashed = false;
    RcCar cars[RC_MAX];
    memset(cars, 0, sizeof(cars));
    RcTree trees[RC_TREES];
    auto new_tree = [&](RcTree &t, float yy) {
        t.r = gk::irand(4, 7);
        t.x = (gk::irand(0, 1) == 0) ? (float)gk::irand(t.r + 2, rx0 - 8) : (float)gk::irand(rx1 + 8, W - t.r - 2);
        t.y = yy;
    };
    for (int i = 0; i < RC_TREES; i++) new_tree(trees[i], (float)gk::irand(0, H));
    auto add_car = [&](int l) {
        for (int i = 0; i < RC_MAX; i++) {
            if (cars[i].on) continue;
            cars[i].on = true;
            cars[i].passed = false;
            cars[i].lane = l;
            cars[i].van = gk::irand(0, 99) < 25;
            cars[i].y = -(float)ch * (cars[i].van ? 1.5f : 1.0f) - 2.0f;
            cars[i].col = COLS[gk::irand(0, 5)];
            return;
        }
    };
    gk::Parts<48> parts;
    gk::Clock clk;

    while (true) {
        k85_input_update();
        const float dt = clk.tick();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_a_pressed()) { k85_wake_screen(); if (lane < 2) { lane++; k85_play_tone(480, 15); } }
        if (k85_btn_b_pressed()) { k85_wake_screen(); if (lane > 0) { lane--; k85_play_tone(480, 15); } }
        frame++;
        if (flash > 0.0f) flash -= dt;
        if (pop_t > 0.0f) pop_t -= dt;
        x += (lane_cx(lane) - x) * gk::clampf(11.0f * dt, 0.0f, 1.0f);

        if (ready > 0.0f) {
            ready -= dt;
        } else if (!crashed) {
            speed += 3.2f * dt;
            if (speed > 235.0f) speed = 235.0f;
            scroll += speed * dt;
            dist += speed * dt / 9.0f;
            spawn_d -= speed * dt;
            if (spawn_d <= 0.0f) {
                if (speed > 130.0f && gk::irand(0, 99) < 30) {
                    int fl = last_free + gk::irand(-1, 1);
                    if (fl < 0) fl = 0;
                    if (fl > 2) fl = 2;
                    for (int l = 0; l < 3; l++) if (l != fl) add_car(l);
                    last_free = fl;
                } else {
                    const int l = gk::irand(0, 2);
                    add_car(l);
                    if (l == last_free) last_free = (l + 1) % 3;
                }
                spawn_d = (float)gk::irand(150, 230) * (1.0f - gk::clampf((speed - 95.0f) / 600.0f, 0.0f, 0.25f));
            }
            for (int i = 0; i < RC_MAX; i++) {
                RcCar &e = cars[i];
                if (!e.on) continue;
                e.y += speed * (1.0f - LANE_F[e.lane]) * dt;
                if (e.y > (float)(H + 10)) { e.on = false; continue; }
                const int eh = e.van ? ch * 3 / 2 : ch;
                const float ex = lane_cx(e.lane);
                if (!e.passed && e.y > (float)(py + ch)) {
                    e.passed = true;
                    overtakes++;
                    if (fabsf(ex - x) < (float)lane_w * 1.05f) {
                        near_bonus += 4;
                        snprintf(pop_txt, sizeof(pop_txt), "+4 NEAR");
                        pop_t = 0.8f;
                        pop_x = x;
                        k85_play_tone(1318, 40);
                    }
                }
                if (fabsf(ex - x) < (float)(cw - 4) && e.y + (float)eh - 3.0f > (float)(py + 3) && e.y + 3.0f < (float)(py + ch - 3)) crashed = true;
            }
            for (int i = 0; i < RC_TREES; i++) {
                trees[i].y += speed * dt;
                if (trees[i].y > (float)(H + 10)) new_tree(trees[i], -10.0f);
            }
            motor_t -= dt;
            if (motor_t <= 0.0f) {
                motor_t = 0.09f;
                k85_play_tone((uint32_t)(110.0f + speed * 0.65f), 85);
            }
            if (crashed) {
                flash = 0.15f;
                parts.burst(x, (float)py + ch * 0.4f, 22, 0xFFB300, 130.0f, 0.7f);
                parts.burst(x, (float)py + ch * 0.4f, 12, 0xBDBDBD, 80.0f, 0.8f);
                k85_play_tone(90, 300);
            }
        } else {
            crash_t += dt;
        }
        parts.update(dt, 40.0f);
        const int score = (int)dist + overtakes * 3 + near_bonus;

        // ---- draw ----
        cv.fillRect(0, 0, W, H, 0x2E7D32);
        const int off = (int)scroll % 48;
        for (int yy = -48 + off; yy < H; yy += 48) cv.fillRect(0, yy, W, 24, 0x338A37);
        if (scenery) {
            for (int i = 0; i < RC_TREES; i++) {
                const int tx = (int)trees[i].x, ty = (int)trees[i].y, r = trees[i].r;
                cv.fillCircle(tx + 2, ty + 2, r, 0x1B5E20);
                cv.fillCircle(tx, ty, r, 0x388E3C);
                cv.fillCircle(tx - 1, ty - 1, r / 2, 0x66BB6A);
            }
        }
        cv.fillRect(rx0, 0, road_w, H, 0x3A3F47);
        const int co = (int)scroll % 20;
        for (int yy = -20 + co; yy < H; yy += 20) {
            cv.fillRect(rx0 - 3, yy, 3, 10, 0xE53935);
            cv.fillRect(rx0 - 3, yy + 10, 3, 10, 0xEEEEEE);
            cv.fillRect(rx1, yy, 3, 10, 0xE53935);
            cv.fillRect(rx1, yy + 10, 3, 10, 0xEEEEEE);
        }
        const int d0 = (int)scroll % 28;
        for (int l = 1; l < 3; l++) {
            for (int yy = -28 + d0; yy < H; yy += 28) cv.fillRect(rx0 + l * lane_w - 1, yy, 2, 14, 0xBDBDBD);
        }
        for (int i = 0; i < RC_MAX; i++) {
            if (!cars[i].on) continue;
            rc_draw_car(cv, (int)lane_cx(cars[i].lane), (int)cars[i].y, cw, cars[i].van ? ch * 3 / 2 : ch, cars[i].col, false, false, frame);
        }
        rc_draw_car(cv, (int)x, py, cw, ch, 0x29B6F6, true, speed > 150.0f && !crashed, frame);
        parts.draw(cv);
        char sb[24];
        snprintf(sb, sizeof(sb), "SCORE %d", score);
        gk::text(cv, 4, 4, sb, 0xFFFFFF);
        snprintf(sb, sizeof(sb), "%d km/h", (int)(speed * 0.6f));
        gk::text(cv, W - gk::text_w(sb) - 4, 4, sb, 0xFFEB3B);
        if (pop_t > 0.0f) gk::text(cv, (int)pop_x - 20, py - 14 - (int)((0.8f - pop_t) * 14.0f), pop_txt, 0xFFD54F);
        if (ready > 0.0f) {
            gk::text(cv, W / 2 - gk::text_w("READY", 2) / 2, H / 2 - 20, "READY", 0xFFFFFF, 2);
            gk::text(cv, W / 2 - gk::text_w("A right   B left") / 2, H / 2 + 2, "A right   B left", 0x9CCBFF);
        }
        if (flash > 0.0f) cv.fillRect(0, 0, W, H, 0xFFFFFF);
        c.push();

        if (crashed && crash_t > 0.7f) {
            const bool newrec = score > s_race_best;
            if (newrec) s_race_best = score;
            char ex[24];
            snprintf(ex, sizeof(ex), "Overtaken: %d", overtakes);
            return gk::end_screen(c, "CRASH!", 0xFF5252, score, s_race_best, newrec, ex);
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void k85_run_race(void) {
    gk::Canvas c;
    if (!gk::need(c)) return;
    while (rc_round(c)) {
    }
}