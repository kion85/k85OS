// K85GAMES2: 2048: tiles slide and merge with an animation, merged tiles pop, new tiles grow in.
// A = next direction (hold A = previous), B = move in the shown direction. Best score key: "2048".
#include "game2048.h"
#include "game_kit.h"
#include "config.h"

#define G_N 4

struct GAnim { int r0, c0, r1, c1, v; };

static uint32_t g_col(int v) {
    switch (v) {
        case 2: return 0xEEE4DA;
        case 4: return 0xEDE0C8;
        case 8: return 0xF2B179;
        case 16: return 0xF59563;
        case 32: return 0xF67C5F;
        case 64: return 0xF65E3B;
        case 128: return 0xEDCF72;
        case 256: return 0xEDCC61;
        case 512: return 0xEDC850;
        case 1024: return 0xEDC53F;
        case 2048: return 0xEDC22E;
        default: return 0x3C3A32;
    }
}
static uint32_t g_txt(int v) { return v <= 4 ? 0x776E65 : 0xF9F6F2; }

static void g_tile(M5Canvas &cv, int x, int y, int size, int v, float scale) {
    const int sz = (int)((float)size * scale);
    if (sz < 2) return;
    const int o = (size - sz) / 2;
    cv.fillRoundRect(x + o, y + o, sz, sz, 4, g_col(v));
    if (sz < 10) return;
    char t[8];
    snprintf(t, sizeof(t), "%d", v);
    const int digits = (int)strlen(t);
    const int ts = (digits * 12 <= sz - 2) ? 2 : 1;
    const int tw = digits * 6 * ts, th = 8 * ts;
    cv.setTextSize(ts);
    cv.setTextColor(g_txt(v));
    cv.setCursor(x + o + (sz - tw) / 2, y + o + (sz - th) / 2);
    cv.print(t);
}

static void g_arrow(M5Canvas &cv, int x, int y, int box, int dir) {
    cv.fillRoundRect(x, y, box, box, 4, 0x2B3048);
    cv.drawRoundRect(x, y, box, box, 4, 0x8A93C0);
    const int cx = x + box / 2, cy = y + box / 2, r = box / 2 - 5;
    const uint32_t a = 0xFFD700;
    switch (dir) {
        case 0: cv.fillTriangle(cx, cy - r, cx - r, cy + r, cx + r, cy + r, a); break;
        case 1: cv.fillTriangle(cx + r, cy, cx - r, cy - r, cx - r, cy + r, a); break;
        case 2: cv.fillTriangle(cx, cy + r, cx - r, cy - r, cx + r, cy - r, a); break;
        default: cv.fillTriangle(cx - r, cy, cx + r, cy - r, cx + r, cy + r, a); break;
    }
}

// 0 up, 1 right, 2 down, 3 left. Fills the animation list and the new grid; true when something moves.
static bool g_plan(int g[G_N][G_N], int dir, int ng[G_N][G_N], bool mg[G_N][G_N], GAnim *an, int *na, int *gain) {
    memset(ng, 0, sizeof(int) * G_N * G_N);
    memset(mg, 0, sizeof(bool) * G_N * G_N);
    *na = 0;
    *gain = 0;
    bool moved = false;
    for (int k = 0; k < G_N; k++) {
        int er[G_N], ec[G_N];
        for (int i = 0; i < G_N; i++) {
            if (dir == 3) { er[i] = k; ec[i] = i; }
            else if (dir == 1) { er[i] = k; ec[i] = G_N - 1 - i; }
            else if (dir == 0) { er[i] = i; ec[i] = k; }
            else { er[i] = G_N - 1 - i; ec[i] = k; }
        }
        int sv[G_N], sr[G_N], sc[G_N], n = 0;
        for (int i = 0; i < G_N; i++) {
            if (g[er[i]][ec[i]]) { sv[n] = g[er[i]][ec[i]]; sr[n] = er[i]; sc[n] = ec[i]; n++; }
        }
        int o = 0, i = 0;
        while (i < n) {
            const bool mergeit = (i + 1 < n && sv[i] == sv[i + 1]);
            const int tr = er[o], tc = ec[o];
            if (mergeit) {
                an[*na] = { sr[i], sc[i], tr, tc, sv[i] };
                (*na)++;
                an[*na] = { sr[i + 1], sc[i + 1], tr, tc, sv[i + 1] };
                (*na)++;
                ng[tr][tc] = sv[i] * 2;
                mg[tr][tc] = true;
                *gain += sv[i] * 2;
                moved = true;
                i += 2;
            } else {
                an[*na] = { sr[i], sc[i], tr, tc, sv[i] };
                (*na)++;
                ng[tr][tc] = sv[i];
                if (sr[i] != tr || sc[i] != tc) moved = true;
                i++;
            }
            o++;
        }
    }
    return moved;
}

static bool g_round(gk::Canvas &c) {
    M5Canvas &cv = c.cv;
    const int W = c.W, H = c.H;
    const bool land = W > H;
    const int gap = 3;
    int gs = land ? (H - 12 - 14) : (W - 8);
    if (!land) {
        const int gh = H - 28 - 18;
        if (gh < gs) gs = gh;
    }
    const int cell = (gs - gap * (G_N + 1)) / G_N;
    gs = cell * G_N + gap * (G_N + 1);
    const int gx = land ? 8 : (W - gs) / 2;
    const int gy = land ? (H - gs) / 2 - 1 : 26;

    int grid[G_N][G_N], ng[G_N][G_N];
    bool mg[G_N][G_N];
    float pop[G_N][G_N];
    memset(grid, 0, sizeof(grid));
    memset(ng, 0, sizeof(ng));
    memset(mg, 0, sizeof(mg));
    for (int r = 0; r < G_N; r++) {
        for (int cc = 0; cc < G_N; cc++) pop[r][cc] = 0.0f;
    }
    GAnim an[16];
    int na = 0, gain = 0, dir = 0, score = 0, sp_r = -1, sp_c = -1;
    float at = 0.0f, sp_t = 1.0f, over_t = 0.0f, win_t = 0.0f;
    bool animating = false, over = false, won = false, newrec = false;
    const int best0 = k85_get_high_score("2048");
    gk::Parts<48> parts;
    gk::Clock clk;

    auto add_tile = [&]() {
        int er[G_N * G_N], ec[G_N * G_N], n = 0;
        for (int r = 0; r < G_N; r++) {
            for (int cc = 0; cc < G_N; cc++) {
                if (grid[r][cc] == 0) { er[n] = r; ec[n] = cc; n++; }
            }
        }
        if (n == 0) return;
        const int k = gk::irand(0, n - 1);
        grid[er[k]][ec[k]] = (gk::irand(0, 9) == 0) ? 4 : 2;
        sp_r = er[k];
        sp_c = ec[k];
        sp_t = 0.0f;
    };
    auto has_moves = [&]() -> bool {
        for (int r = 0; r < G_N; r++) {
            for (int cc = 0; cc < G_N; cc++) {
                if (grid[r][cc] == 0) return true;
                if (cc + 1 < G_N && grid[r][cc] == grid[r][cc + 1]) return true;
                if (r + 1 < G_N && grid[r][cc] == grid[r + 1][cc]) return true;
            }
        }
        return false;
    };
    add_tile();
    add_tile();
    sp_t = 1.0f;

    while (true) {
        k85_input_update();
        const float dt = clk.tick();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            if (score > best0) k85_set_high_score_if_better("2048", score);
            return false;
        }
        if (win_t > 0.0f) win_t -= dt;
        if (sp_t < 1.0f) sp_t += dt;
        for (int r = 0; r < G_N; r++) {
            for (int cc = 0; cc < G_N; cc++) {
                if (pop[r][cc] > 0.0f) pop[r][cc] -= dt;
            }
        }

        if (!over) {
            if (k85_btn_a_pressed()) { k85_wake_screen(); dir = (dir + 1) % 4; }
            const int bk = k85_a_hold_back(2);
            if (bk > 0) dir = ((dir - bk) % 4 + 4) % 4;
            if (k85_btn_b_pressed() && !animating) {
                k85_wake_screen();
                if (g_plan(grid, dir, ng, mg, an, &na, &gain)) {
                    animating = true;
                    at = 0.0f;
                    k85_play_tone(500, 20);
                } else {
                    k85_play_tone(160, 40);
                }
            }
        }
        if (animating) {
            at += dt;
            if (at >= 0.12f) {
                animating = false;
                memcpy(grid, ng, sizeof(grid));
                score += gain;
                for (int r = 0; r < G_N; r++) {
                    for (int cc = 0; cc < G_N; cc++) {
                        if (!mg[r][cc]) continue;
                        pop[r][cc] = 0.18f;
                        parts.burst((float)(gx + gap + cc * (cell + gap) + cell / 2), (float)(gy + gap + r * (cell + gap) + cell / 2), 6, g_col(grid[r][cc]), 60.0f, 0.4f);
                        if (!won && grid[r][cc] == 2048) { won = true; win_t = 2.5f; k85_play_tone(1568, 150); }
                    }
                }
                if (gain > 0) k85_play_tone(880, 40);
                add_tile();
                if (!has_moves()) {
                    over = true;
                    newrec = score > best0;
                    if (newrec) k85_set_high_score_if_better("2048", score);
                    k85_play_tone(130, 260);
                }
            }
        }
        if (over) over_t += dt;
        parts.update(dt, 0.0f);

        // ---- draw ----
        cv.fillRect(0, 0, W, H, 0x0F1420);
        cv.fillRoundRect(gx, gy, gs, gs, 5, 0x3B3F52);
        for (int r = 0; r < G_N; r++) {
            for (int cc = 0; cc < G_N; cc++) {
                cv.fillRoundRect(gx + gap + cc * (cell + gap), gy + gap + r * (cell + gap), cell, cell, 4, 0x4A4E63);
            }
        }
        if (!animating) {
            for (int r = 0; r < G_N; r++) {
                for (int cc = 0; cc < G_N; cc++) {
                    if (!grid[r][cc]) continue;
                    float sc = 1.0f;
                    if (pop[r][cc] > 0.0f) sc = 1.0f + 0.18f * sinf(3.14159f * (1.0f - pop[r][cc] / 0.18f));
                    if (r == sp_r && cc == sp_c && sp_t < 0.14f) sc = sp_t / 0.14f;
                    g_tile(cv, gx + gap + cc * (cell + gap), gy + gap + r * (cell + gap), cell, grid[r][cc], sc);
                }
            }
        } else {
            const float t = gk::clampf(at / 0.12f, 0.0f, 1.0f);
            const float e = t * (2.0f - t);
            for (int i = 0; i < na; i++) {
                const float fr = (float)an[i].r0 + (float)(an[i].r1 - an[i].r0) * e;
                const float fc = (float)an[i].c0 + (float)(an[i].c1 - an[i].c0) * e;
                g_tile(cv, gx + gap + (int)(fc * (float)(cell + gap)), gy + gap + (int)(fr * (float)(cell + gap)), cell, an[i].v, 1.0f);
            }
        }
        parts.draw(cv);
        char sb[24];
        const int best_show = score > best0 ? score : best0;
        if (land) {
            const int px = gx + gs + 12;
            gk::text(cv, px, 10, "2048", 0xEDC22E, 2);
            gk::text(cv, px, 34, "SCORE", 0x8A93C0);
            snprintf(sb, sizeof(sb), "%d", score);
            gk::text(cv, px, 44, sb, 0xFFFFFF, 2);
            gk::text(cv, px, 66, "BEST", 0x8A93C0);
            snprintf(sb, sizeof(sb), "%d", best_show);
            gk::text(cv, px, 76, sb, 0xFFD54F);
            g_arrow(cv, px, 92, 32, dir);
            gk::text(cv, px + 40, 94, "A next dir", 0x8A93C0);
            gk::text(cv, px + 40, 106, "hold A back", 0x8A93C0);
            gk::text(cv, px + 40, 118, "B move", 0x8A93C0);
        } else {
            snprintf(sb, sizeof(sb), "SCORE %d", score);
            gk::text(cv, 4, 4, sb, 0xFFFFFF);
            snprintf(sb, sizeof(sb), "BEST %d", best_show);
            gk::text(cv, 4, 14, sb, 0xFFD54F);
            g_arrow(cv, W - 28, 2, 24, dir);
            gk::text(cv, 4, H - 12, "A dir  B move", 0x8A93C0);
        }
        if (win_t > 0.0f) gk::text(cv, gx + gs / 2 - gk::text_w("2048!", 2) / 2, gy + gs / 2 - 8, "2048!", 0xFFFFFF, 2);
        c.push();

        if (over && over_t > 0.8f) {
            return gk::end_screen(c, "GAME OVER", 0xFF5555, score, k85_get_high_score("2048"), newrec, "No more moves");
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void k85_run_2048(void) {
    gk::Canvas c;
    if (!gk::need(c)) return;
    while (g_round(c)) {
    }
}