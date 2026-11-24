// K85GAMES1: Pong with real physics: paddles have inertia, the bounce angle depends on where the ball hits the paddle,
// the ball speeds up, the AI predicts the ball with a reaction delay and an aiming error. Hold A = up, hold B = down.
#include "pong.h"
#include "game_kit.h"
#include "config.h"
#include "theme.h"

#include <cstdio>
#include <cstring>
#include <cmath>

#define PONG_WIN 5

static bool pong_match(gk::Canvas &c) {
    M5Canvas &cv = c.cv;
    const int W = c.W, H = c.H;
    const float pw = 5.0f, ph = 26.0f;
    const float px = 8.0f, ax = (float)W - 8.0f - pw;
    float py = ((float)H - ph) * 0.5f, pv = 0.0f;
    float ay = py, av = 0.0f;
    float bx = W * 0.5f, by = H * 0.5f, bvx = 0.0f, bvy = 0.0f, speed = 125.0f;
    int ps = 0, as = 0;
    float serve_t = 0.0f, think = 0.0f, ai_y = (float)H * 0.5f, glow_l = 0.0f, glow_r = 0.0f, hint_t = 3.0f, tacc = 0.0f;
    float tx[8], ty[8];
    for (int i = 0; i < 8; i++) { tx[i] = bx; ty[i] = by; }
    int tn = 0;
    gk::Parts<40> parts;
    gk::Clock clk;

    auto serve = [&](int dir) {
        bx = W * 0.5f;
        by = H * 0.5f + (gk::frand() - 0.5f) * 30.0f;
        speed = 125.0f;
        const float a = (gk::frand() - 0.5f) * 0.7f;
        bvx = (float)dir * speed * cosf(a);
        bvy = speed * sinf(a);
        serve_t = 0.9f;
    };
    // keep the speed constant after a hit and never let the ball go almost vertical
    auto norm = [&](float dirx) {
        const float m = sqrtf(bvx * bvx + bvy * bvy);
        if (m < 1.0f) { bvx = dirx * speed; bvy = 0.0f; return; }
        bvx *= speed / m;
        bvy *= speed / m;
        const float minx = speed * 0.35f;
        if (fabsf(bvx) < minx) {
            bvx = dirx * minx;
            const float r2 = speed * speed - minx * minx;
            bvy = (bvy < 0.0f ? -1.0f : 1.0f) * sqrtf(r2 > 0.0f ? r2 : 0.0f);
        }
    };
    serve(gk::irand(0, 1) ? 1 : -1);

    while (true) {
        k85_input_update();
        const float dt = clk.tick();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        tacc += dt;
        hint_t -= dt;
        if (glow_l > 0.0f) glow_l -= dt * 3.0f;
        if (glow_r > 0.0f) glow_r -= dt * 3.0f;

        const bool up = k85_btn_a_is_down(), dn = k85_btn_b_is_down();
        const float target = (up == dn) ? 0.0f : (up ? -150.0f : 150.0f);
        pv += (target - pv) * gk::clampf(14.0f * dt, 0.0f, 1.0f);
        py += pv * dt;
        if (py < 0.0f) { py = 0.0f; pv = 0.0f; }
        if (py > H - ph) { py = H - ph; pv = 0.0f; }

        think -= dt;
        if (think <= 0.0f) {
            think = 0.10f + 0.08f * gk::frand();
            if (bvx > 0.0f && serve_t <= 0.0f) {
                const float tt = (ax - bx) / bvx;
                const float yy = by + bvy * tt;
                float m = fmodf(yy, 2.0f * H);
                if (m < 0.0f) m += 2.0f * H;
                if (m > H) m = 2.0f * H - m;
                float amp = 16.0f - 3.0f * (float)ps;
                if (amp < 3.0f) amp = 3.0f;
                ai_y = m + (gk::frand() * 2.0f - 1.0f) * amp;
            } else {
                ai_y = H * 0.5f + sinf(tacc * 1.3f) * 12.0f;
            }
        }
        const float amax = 105.0f + 10.0f * (float)ps;
        const float want = gk::clampf((ai_y - ph * 0.5f - ay) * 8.0f, -amax, amax);
        av += (want - av) * gk::clampf(10.0f * dt, 0.0f, 1.0f);
        ay += av * dt;
        if (ay < 0.0f) { ay = 0.0f; av = 0.0f; }
        if (ay > H - ph) { ay = H - ph; av = 0.0f; }

        if (serve_t > 0.0f) {
            serve_t -= dt;
        } else {
            const int n = (int)(speed * dt / 2.5f) + 1;
            const float sdt = dt / (float)n;
            for (int s = 0; s < n; s++) {
                bx += bvx * sdt;
                by += bvy * sdt;
                if (by < 3.0f) { by = 3.0f; bvy = fabsf(bvy); k85_play_tone(330, 15); }
                if (by > H - 3.0f) { by = H - 3.0f; bvy = -fabsf(bvy); k85_play_tone(330, 15); }
                if (bvx < 0.0f && bx - 3.0f <= px + pw && bx + 3.0f >= px && by + 3.0f >= py && by - 3.0f <= py + ph) {
                    bx = px + pw + 3.0f;
                    const float rel = gk::clampf((by - (py + ph * 0.5f)) / (ph * 0.5f + 3.0f), -1.0f, 1.0f);
                    speed = gk::clampf(speed * 1.045f, 0.0f, 270.0f);
                    const float a = rel * 0.95f;
                    bvx = speed * cosf(a);
                    bvy = speed * sinf(a) + pv * 0.22f;
                    norm(1.0f);
                    glow_l = 1.0f;
                    k85_play_tone((uint32_t)(640 + gk::irand(-30, 30)), 35);
                    parts.burst(bx, by, 8, 0xFFFFFF, 70.0f, 0.35f);
                } else if (bvx > 0.0f && bx + 3.0f >= ax && bx - 3.0f <= ax + pw && by + 3.0f >= ay && by - 3.0f <= ay + ph) {
                    bx = ax - 3.0f;
                    const float rel = gk::clampf((by - (ay + ph * 0.5f)) / (ph * 0.5f + 3.0f), -1.0f, 1.0f);
                    speed = gk::clampf(speed * 1.045f, 0.0f, 270.0f);
                    const float a = rel * 0.95f;
                    bvx = -speed * cosf(a);
                    bvy = speed * sinf(a) + av * 0.22f;
                    norm(-1.0f);
                    glow_r = 1.0f;
                    k85_play_tone((uint32_t)(420 + gk::irand(-30, 30)), 35);
                    parts.burst(bx, by, 8, 0xFFCC80, 70.0f, 0.35f);
                }
            }
        }
        if (bx < -6.0f) {
            as++;
            k85_play_tone(200, 150);
            parts.burst(0.0f, by, 14, 0xFF5555, 90.0f, 0.6f);
            serve(-1);
        } else if (bx > (float)W + 6.0f) {
            ps++;
            k85_play_tone(900, 150);
            parts.burst((float)W, by, 14, 0x66FF88, 90.0f, 0.6f);
            serve(1);
        }
        tx[tn & 7] = bx;
        ty[tn & 7] = by;
        tn++;
        parts.update(dt, 0.0f);

        // ---- draw ----
        for (int i = 0; i < 6; i++) cv.fillRect(0, H * i / 6, W, H / 6 + 1, gk::mix(0x0B3A4A, 0x06202C, i, 5));
        cv.drawRect(0, 0, W, H, 0x2B6F88);
        for (int y = 4; y < H; y += 10) cv.fillRect(W / 2 - 1, y, 2, 5, 0x3E8AA3);
        cv.drawCircle(W / 2, H / 2, 16, 0x2F6F86);
        char sb[8];
        snprintf(sb, sizeof(sb), "%d", ps);
        gk::text(cv, W / 4 - 6, 6, sb, 0x8FD3E8, 2);
        snprintf(sb, sizeof(sb), "%d", as);
        gk::text(cv, 3 * W / 4 - 6, 6, sb, 0x8FD3E8, 2);
        for (int i = 1; i < 8; i++) {
            const int k = (tn - 1 - i) & 7;
            const int r = 3 - i / 3;
            cv.fillCircle((int)tx[k], (int)ty[k], r > 0 ? r : 1, gk::mix(0xFFFFFF, 0x0B3A4A, i, 8));
        }
        auto paddle = [&](float x, float y, float glow, uint32_t col) {
            if (glow > 0.0f) cv.fillRoundRect((int)x - 2, (int)y - 2, (int)pw + 4, (int)ph + 4, 3, gk::mix(0x0B3A4A, 0xFFFFFF, (int)(glow * 60.0f), 100));
            cv.fillRoundRect((int)x, (int)y, (int)pw, (int)ph, 2, col);
            cv.fillRect((int)x + 1, (int)y + 2, 1, (int)ph - 4, gk::shade(col, 150));
        };
        paddle(px, py, glow_l, 0x66CCFF);
        paddle(ax, ay, glow_r, 0xFF9E80);
        cv.fillCircle((int)bx, (int)by, 3, 0xFFFFFF);
        parts.draw(cv);
        if (hint_t > 0.0f) gk::text(cv, W / 2 - gk::text_w("hold A up   hold B down") / 2, H - 12, "hold A up   hold B down", 0x9CCBFF);
        c.push();

        if (ps >= PONG_WIN || as >= PONG_WIN) {
            const bool won = ps > as;
            const bool newrec = ps > k85_get_high_score("pong");
            k85_set_high_score_if_better("pong", ps);
            char ex[16];
            snprintf(ex, sizeof(ex), "%d - %d", ps, as);
            return gk::end_screen(c, won ? "YOU WIN" : "AI WINS", won ? 0x66FF88 : 0xFF5555, ps, k85_get_high_score("pong"), newrec, ex);
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void k85_run_pong(void) {
    gk::Canvas c;
    if (!gk::need(c)) return;
    while (pong_match(c)) {
    }
}