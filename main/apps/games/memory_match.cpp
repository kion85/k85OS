// K85GAMES2: Memory: cards flip with an animation, shapes instead of letters, particles on a match, a timer.
// A = next card (hold A = previous), B = flip. Best = fewest moves (key "memory").
#include "memory_match.h"
#include "game_kit.h"
#include "config.h"

#define MM_N 16
static const uint32_t MM_COL[8] = { 0xFF5555, 0x55FF55, 0x5599FF, 0xFFFF55, 0xFF55FF, 0x55FFFF, 0xFFAA33, 0xCCCCCC };

static void mm_symbol(M5Canvas &cv, int k, int cx, int cy, int r, uint32_t col, uint32_t bg) {
    switch (k) {
        case 0: cv.fillCircle(cx, cy, r, col); break;
        case 1: cv.fillRect(cx - r, cy - r, 2 * r + 1, 2 * r + 1, col); break;
        case 2: cv.fillTriangle(cx, cy - r, cx - r, cy + r, cx + r, cy + r, col); break;
        case 3:
            cv.fillTriangle(cx, cy - r, cx - r, cy, cx + r, cy, col);
            cv.fillTriangle(cx, cy + r, cx - r, cy, cx + r, cy, col);
            break;
        case 4:
            cv.fillTriangle(cx, cy - r, cx - r, cy + r / 2, cx + r, cy + r / 2, col);
            cv.fillTriangle(cx, cy + r, cx - r, cy - r / 2, cx + r, cy - r / 2, col);
            break;
        case 5:
            cv.fillCircle(cx - r / 2, cy - r / 4, r / 2 + 1, col);
            cv.fillCircle(cx + r / 2, cy - r / 4, r / 2 + 1, col);
            cv.fillTriangle(cx - r, cy, cx + r, cy, cx, cy + r, col);
            break;
        case 6: {
            const int t = r / 3 + 1;
            cv.fillRect(cx - r, cy - t, 2 * r + 1, 2 * t + 1, col);
            cv.fillRect(cx - t, cy - r, 2 * t + 1, 2 * r + 1, col);
            break;
        }
        default:
            cv.fillCircle(cx, cy, r, col);
            cv.fillCircle(cx, cy, r / 2, bg);
            break;
    }
}

static bool mm_round(gk::Canvas &c) {
    M5Canvas &cv = c.cv;
    const int W = c.W, H = c.H;
    const int top = 16, bottom_pad = 12;
    const int cw = W / 4, chh = (H - top - bottom_pad) / 4;
    const int off_x = (W - cw * 4) / 2;

    uint8_t vals[MM_N];
    bool revealed[MM_N], matched[MM_N];
    float fa[MM_N];
    for (int i = 0; i < MM_N; i++) {
        vals[i] = (uint8_t)(i / 2);
        revealed[i] = false;
        matched[i] = false;
        fa[i] = 0.0f;
    }
    for (int i = MM_N - 1; i > 0; i--) {
        const int j = gk::irand(0, i);
        const uint8_t t = vals[i];
        vals[i] = vals[j];
        vals[j] = t;
    }
    int sel = 0, first = -1, second = -1, moves = 0, pairs = 0;
    bool resolving = false;
    float resolve_t = 0.0f, elapsed = 0.0f, done_t = 0.0f;
    gk::Parts<48> parts;
    gk::Clock clk;
    const int best0 = k85_get_high_score("memory");   // 0 = no record yet

    auto step = [&](int s, int d) -> int {
        for (int k = 0; k < MM_N; k++) {
            s = (s + d + MM_N) % MM_N;
            if (!matched[s]) return s;
        }
        return s;
    };
    auto centre_x = [&](int i) -> float { return (float)(off_x + (i % 4) * cw + cw / 2); };
    auto centre_y = [&](int i) -> float { return (float)(top + (i / 4) * chh + chh / 2); };

    while (true) {
        k85_input_update();
        const float dt = clk.tick();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (pairs < 8) {
            elapsed += dt;
            if (k85_btn_a_pressed()) { k85_wake_screen(); sel = step(sel, 1); }
            const int bk = k85_a_hold_back(2);
            for (int i = 0; i < bk; i++) sel = step(sel, -1);
            if (k85_btn_b_pressed()) {
                k85_wake_screen();
                if (!resolving && !matched[sel] && !revealed[sel]) {
                    revealed[sel] = true;
                    k85_play_tone(700, 30);
                    if (first < 0) {
                        first = sel;
                    } else {
                        second = sel;
                        moves++;
                        resolving = true;
                        resolve_t = 0.85f;
                    }
                }
            }
            if (resolving) {
                resolve_t -= dt;
                if (resolve_t <= 0.0f) {
                    if (vals[first] == vals[second]) {
                        matched[first] = true;
                        matched[second] = true;
                        pairs++;
                        parts.burst(centre_x(first), centre_y(first), 10, MM_COL[vals[first]], 80.0f, 0.5f);
                        parts.burst(centre_x(second), centre_y(second), 10, MM_COL[vals[second]], 80.0f, 0.5f);
                        k85_play_tone(1000, 80);
                        if (matched[sel]) sel = step(sel, 1);
                    } else {
                        revealed[first] = false;
                        revealed[second] = false;
                        k85_play_tone(250, 80);
                    }
                    first = -1;
                    second = -1;
                    resolving = false;
                }
            }
        } else {
            done_t += dt;
        }
        for (int i = 0; i < MM_N; i++) {
            const float target = (revealed[i] || matched[i]) ? 1.0f : 0.0f;
            if (fa[i] < target) { fa[i] += dt * 4.0f; if (fa[i] > target) fa[i] = target; }
            else if (fa[i] > target) { fa[i] -= dt * 4.0f; if (fa[i] < target) fa[i] = target; }
        }
        parts.update(dt, 120.0f);

        // ---- draw ----
        cv.fillRect(0, 0, W, H, 0x10142A);
        char sb[40];
        snprintf(sb, sizeof(sb), "MOVES %d", moves);
        gk::text(cv, 4, 4, sb, 0x9CCBFF);
        const int tsec = (int)elapsed;
        snprintf(sb, sizeof(sb), "%d:%02d", tsec / 60, tsec % 60);
        gk::text(cv, W - gk::text_w(sb) - 4, 4, sb, 0xFFFFFF);
        for (int i = 0; i < MM_N; i++) {
            const int x = off_x + (i % 4) * cw, y = top + (i / 4) * chh;
            const int rx = x + 2, ry = y + 2, rw = cw - 4, rh = chh - 4;
            const float scale = fabsf(cosf(3.14159f * fa[i]));
            int w = (int)((float)rw * scale);
            if (w < 2) w = 2;
            const int x0 = rx + (rw - w) / 2;
            if (fa[i] < 0.5f) {
                cv.fillRoundRect(x0, ry, w, rh, 4, 0x333355);
                if (w > 14) {
                    cv.drawRoundRect(x0 + 2, ry + 2, w - 4, rh - 4, 3, 0x4B4B80);
                    cv.fillCircle(x0 + w / 2, ry + rh / 2, 2, 0x4B4B80);
                }
            } else {
                const uint32_t bgc = matched[i] ? 0x1E2A1E : 0xF2F2F2;
                cv.fillRoundRect(x0, ry, w, rh, 4, bgc);
                int r = (rw < rh ? rw : rh) / 2 - 3;
                r = (int)((float)r * scale);
                if (r > 1) mm_symbol(cv, vals[i], x0 + w / 2, ry + rh / 2, r, MM_COL[vals[i]], bgc);
            }
            if (i == sel && pairs < 8) {
                cv.drawRoundRect(x + 1, y + 1, cw - 2, chh - 2, 5, 0xFFFFFF);
                cv.drawRoundRect(x, y, cw, chh, 5, 0xFFFFFF);
            }
        }
        parts.draw(cv);
        if (best0 > 0) snprintf(sb, sizeof(sb), "A next B flip  best %d", best0);
        else snprintf(sb, sizeof(sb), "A next  B flip");
        gk::text(cv, 4, H - 10, sb, 0x8A93C0);
        c.push();

        if (pairs >= 8 && done_t > 0.8f) {
            const bool newrec = (best0 == 0) || moves < best0;
            k85_set_low_score_if_better("memory", moves);
            char ex[24];
            snprintf(ex, sizeof(ex), "Time %d:%02d", tsec / 60, tsec % 60);
            return gk::end_screen(c, "DONE!", 0x66FF88, moves, k85_get_high_score("memory"), newrec, ex, "Moves");
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void k85_run_memory_match(void) {
    gk::Canvas c;
    if (!gk::need(c)) return;
    while (mm_round(c)) {
    }
}