// K85GAMES1: Flappy with real physics (time based), parallax scenery, pipes with caps, feathers and sounds.
// A = flap. Game over: B = retry, A = exit. Best score key: "flappy".
#include "flappy.h"
#include "game_kit.h"
#include "config.h"
#include "theme.h"
#include "list_menu.h"

#include <cstdio>
#include <cstring>
#include <cmath>

#define FL_MAXP 5

struct FlPipe { bool on; bool scored; float x, gap_y, gap; };

static bool fl_hit(float cx, float cy, float r, float x0, float y0, float x1, float y1) {
    const float nx = gk::clampf(cx, x0, x1), ny = gk::clampf(cy, y0, y1);
    const float dx = cx - nx, dy = cy - ny;
    return dx * dx + dy * dy < r * r;
}

static void fl_bird(M5Canvas &cv, int bx, int by, float vy, bool wing_up) {
    cv.fillEllipse(bx, by, 8, 6, 0xFFD54F);
    cv.fillEllipse(bx - 1, by + 2, 5, 3, 0xFFF59D);
    if (wing_up) cv.fillTriangle(bx - 7, by, bx - 1, by - 1, bx - 4, by - 8, 0xFFA000);
    else cv.fillTriangle(bx - 7, by, bx - 1, by + 1, bx - 4, by + 7, 0xFFA000);
    cv.fillCircle(bx + 3, by - 2, 2, 0xFFFFFF);
    cv.fillCircle(bx + 4, by - 2, 1, 0x000000);
    int tip = (int)(vy / 70.0f);
    if (tip > 3) tip = 3;
    if (tip < -3) tip = -3;
    cv.fillTriangle(bx + 7, by - 1, bx + 7, by + 3, bx + 12, by + 1 + tip, 0xFF7043);
}

static bool fl_round(gk::Canvas &c, bool night) {
    M5Canvas &cv = c.cv;
    const int W = c.W, H = c.H;
    const int ground_h = 12, play_h = H - ground_h;
    const float bx = W * 0.22f, R = 6.0f;
    const int pipe_w = 22;
    const float G = 640.0f, FLAP = -178.0f, VMAX = 290.0f, SPACING = 132.0f;
    static const uint32_t TOP[2] = { 0x4FA3E0, 0x050818 };
    static const uint32_t BOT[2] = { 0xBFE9FF, 0x1C2A5C };
    const int sk = night ? 1 : 0;

    float y = play_h * 0.45f, vy = 0.0f, t = 0.0f;
    float since = 100.0f, speed = 100.0f, hill = 0.0f, gnd = 0.0f;
    float wing = 0.0f, flash = 0.0f, dead_t = 0.0f;
    bool started = false, dead = false, newrec = false;
    int score = 0;
    FlPipe pipes[FL_MAXP];
    memset(pipes, 0, sizeof(pipes));
    gk::Parts<48> parts;
    float cl_x[4], cl_y[4];
    for (int i = 0; i < 4; i++) { cl_x[i] = (float)(i * W / 4 + gk::irand(0, 24)); cl_y[i] = (float)gk::irand(10, play_h / 2); }
    gk::Clock clk;

    while (true) {
        k85_input_update();
        const float dt = clk.tick();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            if (score > 0) k85_set_high_score_if_better("flappy", score);
            return false;
        }
        const bool flap = k85_btn_a_pressed();
        if (flap) k85_wake_screen();
        t += dt;
        if (wing > 0.0f) wing -= dt;

        const float sc = dead ? 0.0f : speed;
        hill += sc * 0.22f * dt;
        gnd += sc * dt;
        for (int i = 0; i < 4; i++) {
            cl_x[i] -= sc * 0.12f * dt;
            if (cl_x[i] < -34.0f) { cl_x[i] = (float)(W + 10); cl_y[i] = (float)gk::irand(10, play_h / 2); }
        }

        bool die = false;
        if (!dead) {
            if (!started) {
                y = play_h * 0.45f + sinf(t * 4.0f) * 4.0f;
                if (flap) { started = true; vy = FLAP; wing = 0.25f; k85_play_tone(700, 25); }
            } else {
                if (flap) {
                    vy = FLAP;
                    wing = 0.25f;
                    k85_play_tone(700, 25);
                    parts.burst(bx - 5, y + 3, 3, 0xFFFFFF, 35.0f, 0.35f);
                }
                vy += G * dt;
                if (vy > VMAX) vy = VMAX;
                y += vy * dt;
                speed = 100.0f + (float)(score > 25 ? 25 : score) * 1.2f;
                since += speed * dt;
                if (since >= SPACING) {
                    since -= SPACING;
                    const float gap = 52.0f - (float)(score > 20 ? 20 : score) * 0.7f;
                    for (int i = 0; i < FL_MAXP; i++) {
                        if (pipes[i].on) continue;
                        pipes[i].on = true;
                        pipes[i].scored = false;
                        pipes[i].x = (float)(W + 6);
                        pipes[i].gap = gap;
                        pipes[i].gap_y = gap * 0.5f + 14.0f + gk::frand() * ((float)play_h - gap - 28.0f);
                        break;
                    }
                }
                for (int i = 0; i < FL_MAXP; i++) {
                    FlPipe &p = pipes[i];
                    if (!p.on) continue;
                    p.x -= speed * dt;
                    if (p.x + pipe_w + 2 < 0.0f) { p.on = false; continue; }
                    if (!p.scored && p.x + pipe_w < bx - R) { p.scored = true; score++; k85_play_tone(1175, 50); }
                    const float gt = p.gap_y - p.gap * 0.5f, gb = p.gap_y + p.gap * 0.5f;
                    if (fl_hit(bx, y, R, p.x, -50.0f, p.x + pipe_w, gt) ||
                        fl_hit(bx, y, R, p.x, gb, p.x + pipe_w, (float)play_h + 50.0f) ||
                        fl_hit(bx, y, R, p.x - 2.0f, gt - 8.0f, p.x + pipe_w + 2.0f, gt) ||
                        fl_hit(bx, y, R, p.x - 2.0f, gb, p.x + pipe_w + 2.0f, gb + 8.0f)) die = true;
                }
                if (y + R >= (float)play_h) { y = (float)play_h - R; die = true; }
                if (y - R < 0.0f) { y = R; if (vy < 0.0f) vy = 0.0f; }
            }
        } else {
            dead_t += dt;
            vy += G * dt;
            if (vy > VMAX) vy = VMAX;
            y += vy * dt;
            if (y >= (float)play_h - R) { y = (float)play_h - R; vy = 0.0f; }
        }
        if (die) {
            dead = true;
            vy = -90.0f;
            flash = 0.12f;
            newrec = score > k85_get_high_score("flappy");
            k85_set_high_score_if_better("flappy", score);
            k85_play_tone(160, 220);
            parts.burst(bx, y, 18, 0xFFEB3B, 95.0f, 0.8f);
        }
        parts.update(dt, 260.0f);

        // ---- draw ----
        for (int i = 0; i < 8; i++) {
            const int y0 = play_h * i / 8, y1 = play_h * (i + 1) / 8;
            cv.fillRect(0, y0, W, y1 - y0 + 1, gk::mix(TOP[sk], BOT[sk], i, 7));
        }
        if (night) {
            for (int i = 0; i < 18; i++) {
                if ((((int)(t * 3.0f)) + i) % 4 != 0) cv.drawPixel((i * 47 + 13) % W, (i * 23 + 5) % (play_h / 2), 0xFFFFFF);
            }
            cv.fillCircle(W - 34, 24, 10, 0xEEF2FF);
            cv.fillCircle(W - 29, 21, 9, gk::mix(TOP[1], BOT[1], 1, 7));
        } else {
            cv.fillCircle(W - 30, 24, 11, 0xFFF59D);
            cv.fillCircle(W - 30, 24, 8, 0xFFEE58);
        }
        const uint32_t cloud = night ? 0x2B3A6B : 0xFFFFFF;
        for (int i = 0; i < 4; i++) {
            const int ccx = (int)cl_x[i], ccy = (int)cl_y[i];
            cv.fillCircle(ccx, ccy, 6, cloud);
            cv.fillCircle(ccx + 7, ccy + 2, 5, cloud);
            cv.fillCircle(ccx - 7, ccy + 2, 4, cloud);
            cv.fillRect(ccx - 7, ccy + 2, 15, 5, cloud);
        }
        const uint32_t h1 = night ? 0x16264A : 0x7BC47F, h2 = night ? 0x0F1B38 : 0x4E9F57;
        for (int x = 0; x < W; x += 3) {
            const float a = (float)x + hill;
            const int hg1 = 20 + (int)(10.0f * sinf(a * 0.035f) + 5.0f * sinf(a * 0.09f));
            cv.fillRect(x, play_h - hg1, 3, hg1, h1);
            const float b = (float)x + hill * 1.7f + 60.0f;
            const int hg2 = 9 + (int)(6.0f * sinf(b * 0.05f) + 3.0f * sinf(b * 0.13f));
            cv.fillRect(x, play_h - hg2, 3, hg2, h2);
        }
        const uint32_t pc = night ? 0x2E8B62 : 0x4CAF50;
        for (int i = 0; i < FL_MAXP; i++) {
            const FlPipe &p = pipes[i];
            if (!p.on) continue;
            const int x = (int)p.x, gt = (int)(p.gap_y - p.gap * 0.5f), gb = (int)(p.gap_y + p.gap * 0.5f);
            cv.fillRect(x, 0, pipe_w, gt - 8, pc);
            cv.fillRect(x, 0, 4, gt - 8, gk::shade(pc, 135));
            cv.fillRect(x + pipe_w - 5, 0, 5, gt - 8, gk::shade(pc, 70));
            cv.fillRect(x - 2, gt - 8, pipe_w + 4, 8, pc);
            cv.fillRect(x - 2, gt - 8, 4, 8, gk::shade(pc, 135));
            cv.fillRect(x + pipe_w - 3, gt - 8, 5, 8, gk::shade(pc, 70));
            cv.drawRect(x - 2, gt - 8, pipe_w + 4, 8, 0x1B3A1E);
            cv.fillRect(x, gb + 8, pipe_w, play_h - gb - 8, pc);
            cv.fillRect(x, gb + 8, 4, play_h - gb - 8, gk::shade(pc, 135));
            cv.fillRect(x + pipe_w - 5, gb + 8, 5, play_h - gb - 8, gk::shade(pc, 70));
            cv.fillRect(x - 2, gb, pipe_w + 4, 8, pc);
            cv.fillRect(x - 2, gb, 4, 8, gk::shade(pc, 135));
            cv.fillRect(x + pipe_w - 3, gb, 5, 8, gk::shade(pc, 70));
            cv.drawRect(x - 2, gb, pipe_w + 4, 8, 0x1B3A1E);
        }
        cv.fillRect(0, play_h, W, ground_h, night ? 0x6B5E3A : 0xD7C58B);
        cv.fillRect(0, play_h, W, 3, night ? 0x2F6B3A : 0x6BBF59);
        for (int x = -((int)gnd % 12); x < W; x += 12) cv.drawLine(x, play_h + 3, x + 6, play_h + ground_h - 1, night ? 0x4E4429 : 0xB8A56A);
        fl_bird(cv, (int)bx, (int)y, vy, wing > 0.0f);
        parts.draw(cv);
        char sb[12];
        snprintf(sb, sizeof(sb), "%d", score);
        gk::text(cv, W / 2 - gk::text_w(sb, 2) / 2, 6, sb, 0xFFFFFF, 2);
        if (!started) gk::text(cv, W / 2 - gk::text_w("A = flap") / 2, play_h / 2 + 22, "A = flap", 0xFFFFFF);
        if (flash > 0.0f) { cv.fillRect(0, 0, W, H, 0xFFFFFF); flash -= dt; }
        c.push();

        if (dead && dead_t > 0.5f && y >= (float)play_h - R - 0.5f) {
            const char *medal = score >= 20 ? "Gold medal" : (score >= 10 ? "Silver medal" : (score >= 5 ? "Bronze medal" : nullptr));
            return gk::end_screen(c, "GAME OVER", 0xFF5555, score, k85_get_high_score("flappy"), newrec, medal);
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void k85_run_flappy(void) {
    const char *bg_items[] = { "Sky (classic)", "Night" };
    const int choice = k85_run_list_menu("FLAPPY - Background", bg_items, 2, nullptr);
    if (choice < 0) return;
    gk::Canvas c;
    if (!gk::need(c)) return;
    while (fl_round(c, choice == 1)) {
    }
}