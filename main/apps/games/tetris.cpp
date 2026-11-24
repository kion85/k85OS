// K85GAMES2: Tetris: all 7 pieces (7-bag), ghost piece, lock delay, line-clear flash, levels, next piece.
// A click = left, B click = right, hold A = rotate, hold B = hard drop. Game over: B = retry, A = exit.
// Best score key: "tetris".
#include "tetris.h"
#include "game_kit.h"
#include "config.h"

#define TT_C 10
#define TT_R 16

struct TtShape { int8_t d[4][2]; };
static const TtShape TT_SH[7] = {
    {{{0, -1}, {0, 0}, {0, 1}, {0, 2}}},    // I
    {{{0, 0}, {0, 1}, {1, 0}, {1, 1}}},     // O
    {{{0, -1}, {0, 0}, {0, 1}, {1, 0}}},    // T
    {{{0, 0}, {0, 1}, {1, -1}, {1, 0}}},    // S
    {{{0, -1}, {0, 0}, {1, 0}, {1, 1}}},    // Z
    {{{0, -1}, {0, 0}, {0, 1}, {1, 1}}},    // J
    {{{0, -1}, {0, 0}, {0, 1}, {1, -1}}},   // L
};
static const uint32_t TT_COL[8] = { 0x000000, 0x00E5FF, 0xFFD600, 0xAA00FF, 0x00E676, 0xFF1744, 0x2979FF, 0xFF9100 };
static const int TT_PTS[5] = { 0, 10, 30, 60, 100 };

struct TtP { int t; int pr, pc; int8_t d[4][2]; };

static void tt_cell(M5Canvas &cv, int x, int y, int s, uint32_t col) {
    cv.fillRect(x, y, s, s, col);
    if (s >= 5) {
        cv.fillRect(x, y, s, 1, gk::shade(col, 140));
        cv.fillRect(x, y, 1, s, gk::shade(col, 140));
        cv.fillRect(x, y + s - 1, s, 1, gk::shade(col, 60));
        cv.fillRect(x + s - 1, y, 1, s, gk::shade(col, 60));
    }
}

static void tt_preview(M5Canvas &cv, int t, int x0, int y0, int s) {
    int minr = 9, maxr = -9, minc = 9, maxc = -9;
    for (int i = 0; i < 4; i++) {
        const int r = TT_SH[t].d[i][0], c = TT_SH[t].d[i][1];
        if (r < minr) minr = r;
        if (r > maxr) maxr = r;
        if (c < minc) minc = c;
        if (c > maxc) maxc = c;
    }
    const int w = (maxc - minc + 1) * s, h = (maxr - minr + 1) * s;
    const int ox = x0 + (4 * s - w) / 2, oy = y0 + (2 * s - h) / 2;
    for (int i = 0; i < 4; i++) {
        tt_cell(cv, ox + (TT_SH[t].d[i][1] - minc) * s, oy + (TT_SH[t].d[i][0] - minr) * s, s, TT_COL[t + 1]);
    }
}

static bool tt_round(gk::Canvas &c) {
    M5Canvas &cv = c.cv;
    const int W = c.W, H = c.H;
    const bool land = W > H;
    int cell = land ? (H - 18) / TT_R : (W - 4) / TT_C;
    if (!land) {
        const int chh = (H - 40) / TT_R;
        if (chh < cell) cell = chh;
    }
    if (cell < 4) cell = 4;
    const int bw = cell * TT_C, bh = cell * TT_R;
    const int bx = land ? 8 : (W - bw) / 2;
    const int by = land ? (H - bh) / 2 + 6 : 26;

    uint8_t b[TT_R][TT_C];
    memset(b, 0, sizeof(b));
    bool full_row[TT_R];
    memset(full_row, 0, sizeof(full_row));
    int bag[7], bag_n = 0;
    int score = 0, lines = 0, level = 0, clear_n = 0, resets = 0;
    float gtime = 0.0f, lock_t = 0.0f, clear_t = 0.0f, over_t = 0.0f, flash = 0.0f, hint = 6.0f;
    bool over = false, clearing = false, newrec = false;
    gk::Clock clk;
    const int best0 = k85_get_high_score("tetris");

    auto next_type = [&]() -> int {
        if (bag_n == 0) {
            for (int i = 0; i < 7; i++) bag[i] = i;
            for (int i = 6; i > 0; i--) {
                const int j = gk::irand(0, i);
                const int tmp = bag[i];
                bag[i] = bag[j];
                bag[j] = tmp;
            }
            bag_n = 7;
        }
        return bag[--bag_n];
    };
    auto make = [&](int t) -> TtP {
        TtP p;
        p.t = t;
        p.pr = 1;
        p.pc = 4;
        for (int i = 0; i < 4; i++) { p.d[i][0] = TT_SH[t].d[i][0]; p.d[i][1] = TT_SH[t].d[i][1]; }
        return p;
    };
    auto fits = [&](const TtP &p) -> bool {
        for (int i = 0; i < 4; i++) {
            const int rr = p.pr + p.d[i][0], cc = p.pc + p.d[i][1];
            if (cc < 0 || cc >= TT_C || rr >= TT_R) return false;
            if (rr >= 0 && b[rr][cc]) return false;
        }
        return true;
    };
    TtP cur = make(next_type());
    int next_t = next_type();

    auto spawn = [&]() {
        cur = make(next_t);
        next_t = next_type();
        gtime = 0.0f;
        lock_t = 0.0f;
        resets = 0;
        if (!fits(cur)) {
            over = true;
            flash = 0.12f;
            newrec = score > best0;
            if (newrec) k85_set_high_score_if_better("tetris", score);
            k85_play_tone(130, 260);
        }
    };
    auto lock_piece = [&]() {
        for (int i = 0; i < 4; i++) {
            const int rr = cur.pr + cur.d[i][0], cc = cur.pc + cur.d[i][1];
            if (rr >= 0 && rr < TT_R && cc >= 0 && cc < TT_C) b[rr][cc] = (uint8_t)(cur.t + 1);
        }
        int n = 0;
        for (int r = 0; r < TT_R; r++) {
            bool full = true;
            for (int cc = 0; cc < TT_C; cc++) {
                if (!b[r][cc]) { full = false; break; }
            }
            full_row[r] = full;
            if (full) n++;
        }
        k85_play_tone(220, 25);
        if (n > 0) {
            clearing = true;
            clear_t = 0.30f;
            clear_n = n > 4 ? 4 : n;
            k85_play_tone((uint32_t)(660 + 110 * n), 80);
        } else {
            spawn();
        }
    };
    auto finish_clear = [&]() {
        int w = TT_R - 1;
        for (int r = TT_R - 1; r >= 0; r--) {
            if (full_row[r]) continue;
            if (w != r) memcpy(b[w], b[r], TT_C);
            w--;
        }
        for (; w >= 0; w--) memset(b[w], 0, TT_C);
        const int old_level = level;
        score += TT_PTS[clear_n] * (level + 1);
        lines += clear_n;
        level = lines / 10;
        if (level > old_level) k85_play_tone(1319, 120);
        clearing = false;
        memset(full_row, 0, sizeof(full_row));
        spawn();
    };
    auto grounded_reset = [&]() {
        TtP below = cur;
        below.pr++;
        if (!fits(below) && resets < 12) { lock_t = 0.0f; resets++; }
    };

    while (true) {
        k85_input_update();
        const float dt = clk.tick();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            if (score > best0) k85_set_high_score_if_better("tetris", score);
            return false;
        }
        if (hint > 0.0f) hint -= dt;
        if (flash > 0.0f) flash -= dt;

        const bool ac = M5.BtnA.wasClicked() && !M5.BtnB.isPressed();
        const bool bc = M5.BtnB.wasClicked() && !M5.BtnA.isPressed();
        const bool ha = M5.BtnA.wasHold() && !M5.BtnB.isPressed();
        const bool hb = M5.BtnB.wasHold() && !M5.BtnA.isPressed();
        if (ac || bc || ha || hb) k85_wake_screen();

        if (over) {
            over_t += dt;
        } else if (clearing) {
            clear_t -= dt;
            if (clear_t <= 0.0f) finish_clear();
        } else {
            if (ac || bc) {
                TtP q = cur;
                q.pc += ac ? -1 : 1;
                if (fits(q)) { cur = q; grounded_reset(); k85_play_tone(420, 12); }
            }
            if (ha && cur.t != 1) {
                TtP q = cur;
                for (int i = 0; i < 4; i++) {
                    q.d[i][0] = cur.d[i][1];
                    q.d[i][1] = (int8_t)(-cur.d[i][0]);
                }
                static const int kick[5] = { 0, -1, 1, -2, 2 };
                for (int k = 0; k < 5; k++) {
                    TtP r = q;
                    r.pc += kick[k];
                    if (fits(r)) { cur = r; grounded_reset(); k85_play_tone(700, 15); break; }
                }
            }
            if (hb) {
                for (;;) {
                    TtP q = cur;
                    q.pr++;
                    if (!fits(q)) break;
                    cur = q;
                }
                lock_piece();
            }
            if (!over && !clearing) {
                gtime += dt;
                float gi = 0.72f - (float)level * 0.06f;
                if (gi < 0.07f) gi = 0.07f;
                while (gtime >= gi) {
                    gtime -= gi;
                    TtP q = cur;
                    q.pr++;
                    if (fits(q)) cur = q; else { gtime = 0.0f; break; }
                }
                TtP below = cur;
                below.pr++;
                if (!fits(below)) {
                    lock_t += dt;
                    if (lock_t >= 0.45f) lock_piece();
                } else {
                    lock_t = 0.0f;
                }
            }
        }

        // ---- draw ----
        cv.fillRect(0, 0, W, H, 0x0A1020);
        cv.fillRect(bx - 2, by - 2, bw + 4, bh + 4, 0x2A3555);
        cv.fillRect(bx, by, bw, bh, 0x05080F);
        for (int r = 0; r < TT_R; r++) {
            for (int cc = 0; cc < TT_C; cc++) cv.drawPixel(bx + cc * cell + cell / 2, by + r * cell + cell / 2, 0x151C33);
        }
        for (int r = 0; r < TT_R; r++) {
            for (int cc = 0; cc < TT_C; cc++) {
                if (!b[r][cc]) continue;
                uint32_t col = TT_COL[b[r][cc]];
                if (clearing && full_row[r] && (((int)(clear_t * 24.0f)) & 1)) col = 0xFFFFFF;
                tt_cell(cv, bx + cc * cell, by + r * cell, cell, col);
            }
        }
        if (!over && !clearing) {
            TtP g = cur;
            for (;;) {
                TtP n = g;
                n.pr++;
                if (!fits(n)) break;
                g = n;
            }
            if (g.pr != cur.pr) {
                for (int i = 0; i < 4; i++) {
                    const int rr = g.pr + g.d[i][0], cc = g.pc + g.d[i][1];
                    if (rr >= 0) cv.drawRect(bx + cc * cell, by + rr * cell, cell, cell, gk::shade(TT_COL[cur.t + 1], 70));
                }
            }
        }
        if (!clearing) {
            for (int i = 0; i < 4; i++) {
                const int rr = cur.pr + cur.d[i][0], cc = cur.pc + cur.d[i][1];
                if (rr >= 0 && rr < TT_R && cc >= 0 && cc < TT_C) tt_cell(cv, bx + cc * cell, by + rr * cell, cell, TT_COL[cur.t + 1]);
            }
        }
        char sb[28];
        const int best_show = score > best0 ? score : best0;
        if (land) {
            const int px = bx + bw + 14;
            gk::text(cv, px, 14, "SCORE", 0x7F8FB5);
            snprintf(sb, sizeof(sb), "%d", score);
            gk::text(cv, px, 24, sb, 0xFFFFFF, 2);
            gk::text(cv, px, 46, "BEST", 0x7F8FB5);
            snprintf(sb, sizeof(sb), "%d", best_show);
            gk::text(cv, px, 56, sb, 0xFFD54F);
            snprintf(sb, sizeof(sb), "LEVEL %d", level + 1);
            gk::text(cv, px, 74, sb, 0xA5D6A7);
            snprintf(sb, sizeof(sb), "LINES %d", lines);
            gk::text(cv, px, 86, sb, 0xA5D6A7);
            gk::text(cv, W - 52, 14, "NEXT", 0x7F8FB5);
            tt_preview(cv, next_t, W - 52, 26, 8);
            gk::text(cv, px, H - 34, "A left  B right", 0x7F8FB5);
            gk::text(cv, px, H - 23, "hold A rotate", 0x7F8FB5);
            gk::text(cv, px, H - 12, "hold B drop", 0x7F8FB5);
        } else {
            snprintf(sb, sizeof(sb), "SCORE %d", score);
            gk::text(cv, 4, 3, sb, 0xFFFFFF);
            snprintf(sb, sizeof(sb), "LV %d  LN %d", level + 1, lines);
            gk::text(cv, 4, 13, sb, 0xA5D6A7);
            tt_preview(cv, next_t, W - 40, 3, 6);
            snprintf(sb, sizeof(sb), "BEST %d", best_show);
            gk::text(cv, 4, H - 22, sb, 0xFFD54F);
            gk::text(cv, 4, H - 12, "A< B> hA:rot hB:drop", 0x7F8FB5);
        }
        if (hint > 0.0f && land) gk::text(cv, bx + 2, by + bh / 2 - 4, "A<  B>", 0x4F5F8A);
        if (flash > 0.0f) cv.fillRect(0, 0, W, H, 0xFFFFFF);
        c.push();

        if (over && over_t > 0.6f) {
            char ex[24];
            snprintf(ex, sizeof(ex), "Lines %d  Lv %d", lines, level + 1);
            return gk::end_screen(c, "GAME OVER", 0xFF5555, score, k85_get_high_score("tetris"), newrec, ex);
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void k85_run_tetris(void) {
    gk::Canvas c;
    if (!gk::need(c)) return;
    while (tt_round(c)) {
    }
}