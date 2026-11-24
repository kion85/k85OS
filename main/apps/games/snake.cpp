// K85GAMES2: Snake: smooth movement between cells, a body with a gradient and eyes, apples (a timed golden one),
// speed grows with the score, particles and sounds. Tilt the stick, or A = turn left, B = turn right.
// Game over: B = retry, A = exit. Best score key: "snake".
#include "snake.h"
#include "game_kit.h"
#include "config.h"
#include "esp_attr.h"

#define SN_MAX 400

static EXT_RAM_BSS_ATTR int16_t s_cx[SN_MAX], s_cy[SN_MAX], s_px[SN_MAX], s_py[SN_MAX];

static void sn_apple(M5Canvas &cv, int x, int y, int cell, bool gold, int frame) {
    const int cx = x + cell / 2, cy = y + cell / 2;
    if (gold) {
        const uint32_t g = ((frame >> 2) & 1) ? 0xFFD54F : 0xFFC107;
        cv.fillCircle(cx, cy, cell / 2 - 1, g);
        cv.fillCircle(cx - 1, cy - 1, 1, 0xFFFFFF);
        if ((frame >> 3) & 1) {
            cv.drawFastHLine(cx - cell / 2, cy, cell, 0xFFF59D);
            cv.drawFastVLine(cx, cy - cell / 2, cell, 0xFFF59D);
        }
    } else {
        cv.fillCircle(cx, cy + 1, cell / 2 - 1, 0xE53935);
        cv.fillCircle(cx - 1, cy, 1, 0xFF8A80);
        cv.drawLine(cx, cy - cell / 2 + 1, cx + 2, cy - cell / 2 - 1, 0x43A047);
    }
}

static bool sn_round(gk::Canvas &c) {
    M5Canvas &cv = c.cv;
    const int W = c.W, H = c.H;
    const int cell = 8, top = 18;
    const int cols = W / cell;
    const int rows = (H - top - 2) / cell;
    const int ox = (W - cols * cell) / 2;
    const int oy = top + (H - top - rows * cell) / 2;

    int len = 3, dx = 1, dy = 0, pdx = 1, pdy = 0, ltx = 0, lty = 0;
    for (int i = 0; i < len; i++) {
        s_cx[i] = (int16_t)(cols / 2 - i);
        s_cy[i] = (int16_t)(rows / 2);
        s_px[i] = s_cx[i];
        s_py[i] = s_cy[i];
    }
    int ax = 0, ay = 0, score = 0, frame = 0;
    bool gold = false, dead = false, newrec = false;
    float gold_t = 0.0f, acc = 0.0f, dead_t = 0.0f, flash = 0.0f, hint = 5.0f;
    gk::Parts<48> parts;
    gk::Clock clk;
    const int best0 = k85_get_high_score("snake");

    auto step_len = [&]() -> float {
        const float s = 0.19f - (float)score * 0.0035f;
        return s < 0.085f ? 0.085f : s;
    };
    auto place = [&]() {
        for (int tries = 0; tries < 300; tries++) {
            const int x = gk::irand(0, cols - 1), y = gk::irand(0, rows - 1);
            bool used = false;
            for (int i = 0; i < len; i++) {
                if (s_cx[i] == x && s_cy[i] == y) { used = true; break; }
            }
            if (used) continue;
            ax = x;
            ay = y;
            gold = (score >= 4 && gk::irand(0, 99) < 18);
            gold_t = 6.0f;
            return;
        }
    };
    auto try_dir = [&](int ndx, int ndy) {
        if (ndx == -dx && ndy == -dy) return;
        pdx = ndx;
        pdy = ndy;
    };
    place();

    while (true) {
        k85_input_update();
        const float dt = clk.tick();
        frame++;
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            if (score > best0) k85_set_high_score_if_better("snake", score);
            return false;
        }
        if (hint > 0.0f) hint -= dt;
        if (flash > 0.0f) flash -= dt;

        if (!dead) {
            if (k85_btn_a_pressed()) { k85_wake_screen(); try_dir(pdy, -pdx); }
            if (k85_btn_b_pressed()) { k85_wake_screen(); try_dir(-pdy, pdx); }
            float tax = 0.0f, tay = 0.0f, taz = 0.0f;
            M5.Imu.getAccel(&tax, &tay, &taz);
            int tx = 0, ty = 0;
            if (fabsf(tax) > fabsf(tay)) {
                if (tax > 0.35f) tx = 1; else if (tax < -0.35f) tx = -1;
            } else {
                if (tay > 0.35f) ty = 1; else if (tay < -0.35f) ty = -1;
            }
            if ((tx || ty) && (tx != ltx || ty != lty)) try_dir(tx, ty);
            ltx = tx;
            lty = ty;

            if (gold) {
                gold_t -= dt;
                if (gold_t <= 0.0f) { gold = false; place(); }
            }
            acc += dt;
            float sl = step_len();
            while (acc >= sl && !dead) {
                acc -= sl;
                dx = pdx;
                dy = pdy;
                const int nx = s_cx[0] + dx, ny = s_cy[0] + dy;
                const bool eat = (nx == ax && ny == ay);
                bool hit = (nx < 0 || nx >= cols || ny < 0 || ny >= rows);
                if (!hit) {
                    const int lim = eat ? len : len - 1;
                    for (int i = 0; i < lim; i++) {
                        if (s_cx[i] == nx && s_cy[i] == ny) { hit = true; break; }
                    }
                }
                if (hit) {
                    dead = true;
                    flash = 0.12f;
                    newrec = score > best0;
                    if (newrec) k85_set_high_score_if_better("snake", score);
                    k85_play_tone(140, 250);
                    parts.burst((float)(ox + s_cx[0] * cell + cell / 2), (float)(oy + s_cy[0] * cell + cell / 2), 20, 0x7CFC4A, 100.0f, 0.8f);
                    break;
                }
                for (int i = 0; i < len; i++) { s_px[i] = s_cx[i]; s_py[i] = s_cy[i]; }
                for (int i = len - 1; i >= 1; i--) { s_cx[i] = s_px[i - 1]; s_cy[i] = s_py[i - 1]; }
                s_cx[0] = (int16_t)nx;
                s_cy[0] = (int16_t)ny;
                if (eat && len < SN_MAX) {
                    s_cx[len] = s_px[len - 1];
                    s_cy[len] = s_py[len - 1];
                    s_px[len] = s_px[len - 1];
                    s_py[len] = s_py[len - 1];
                    len++;
                }
                if (eat) {
                    score += gold ? 3 : 1;
                    parts.burst((float)(ox + nx * cell + cell / 2), (float)(oy + ny * cell + cell / 2), gold ? 14 : 8, gold ? 0xFFD54F : 0xFF5252, 70.0f, 0.5f);
                    k85_play_tone(gold ? 1319 : (uint32_t)(740 + (len % 8) * 40), 35);
                    place();
                }
                sl = step_len();
            }
        } else {
            dead_t += dt;
        }
        parts.update(dt, 0.0f);

        // ---- draw ----
        cv.fillRect(0, 0, W, H, 0x0B1712);
        cv.fillRect(ox, oy, cols * cell, rows * cell, 0x12261C);
        for (int r = 0; r < rows; r++) {
            for (int cc = (r & 1); cc < cols; cc += 2) cv.fillRect(ox + cc * cell, oy + r * cell, cell, cell, 0x15291F);
        }
        cv.drawRect(ox - 1, oy - 1, cols * cell + 2, rows * cell + 2, 0x2E7D4F);
        sn_apple(cv, ox + ax * cell, oy + ay * cell, cell, gold, frame);
        const float f = dead ? 1.0f : gk::clampf(acc / step_len(), 0.0f, 1.0f);
        int hx = 0, hy = 0;
        for (int i = len - 1; i >= 0; i--) {
            const float fx = (float)s_px[i] + (float)(s_cx[i] - s_px[i]) * f;
            const float fy = (float)s_py[i] + (float)(s_cy[i] - s_py[i]) * f;
            const int x = ox + (int)(fx * (float)cell + 0.5f), y = oy + (int)(fy * (float)cell + 0.5f);
            const uint32_t col = gk::mix(0x1B5E20, 0x7CFC4A, len - 1 - i, len > 1 ? len - 1 : 1);
            cv.fillRoundRect(x, y, cell - 1, cell - 1, 2, col);
            if (i > 0 && (i & 1)) cv.fillRect(x + 2, y + 2, cell - 5, cell - 5, gk::shade(col, 85));
            if (i == 0) { hx = x; hy = y; }
        }
        {
            const int hcx = hx + cell / 2, hcy = hy + cell / 2;
            const int e1x = hcx + dx - dy * 2, e1y = hcy + dy + dx * 2;
            const int e2x = hcx + dx + dy * 2, e2y = hcy + dy - dx * 2;
            cv.fillRect(e1x - 1, e1y - 1, 2, 2, 0xFFFFFF);
            cv.fillRect(e2x - 1, e2y - 1, 2, 2, 0xFFFFFF);
            cv.drawPixel(e1x + dx, e1y + dy, 0x000000);
            cv.drawPixel(e2x + dx, e2y + dy, 0x000000);
            if (!dead && ((frame >> 4) & 1)) cv.drawLine(hcx + dx * 4, hcy + dy * 4, hcx + dx * 7, hcy + dy * 7, 0xFF1744);
        }
        parts.draw(cv);
        char sb[24];
        snprintf(sb, sizeof(sb), "SCORE %d", score);
        gk::text(cv, 4, 5, sb, 0xFFFFFF);
        snprintf(sb, sizeof(sb), "BEST %d", score > best0 ? score : best0);
        gk::text(cv, W - gk::text_w(sb) - 4, 5, sb, 0xFFD54F);
        if (gold) cv.fillRect(W / 2 - 20, 6, (int)(40.0f * gk::clampf(gold_t, 0.0f, 6.0f) / 6.0f), 4, 0xFFC107);
        if (hint > 0.0f) gk::text(cv, W / 2 - gk::text_w("tilt   or   A left  B right") / 2, H - 11, "tilt   or   A left  B right", 0x9CCBFF);
        if (flash > 0.0f) cv.fillRect(0, 0, W, H, 0xFFFFFF);
        c.push();

        if (dead && dead_t > 0.7f) {
            char ex[24];
            snprintf(ex, sizeof(ex), "Length %d", len);
            return gk::end_screen(c, "GAME OVER", 0xFF5555, score, k85_get_high_score("snake"), newrec, ex);
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void k85_run_snake(void) {
    gk::Canvas c;
    if (!gk::need(c)) return;
    while (sn_round(c)) {
    }
}