#pragma once
// K85GAMES1: shared helpers for the games: off-screen canvas, colours, particles, text with a shadow, the end-of-game panel
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "M5Unified.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "power.h"
#include "sound.h"

namespace gk {

static inline float frand(void) { return (float)(esp_random() & 0xFFFF) / 65535.0f; }
static inline int irand(int a, int b) { return b <= a ? a : a + (int)(esp_random() % (uint32_t)(b - a + 1)); }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static inline uint32_t mix(uint32_t a, uint32_t b, int t, int n) {
    if (n <= 0) return a;
    const int ar = (int)((a >> 16) & 255), ag = (int)((a >> 8) & 255), ab = (int)(a & 255);
    const int br = (int)((b >> 16) & 255), bgc = (int)((b >> 8) & 255), bb = (int)(b & 255);
    return ((uint32_t)(ar + (br - ar) * t / n) << 16) | ((uint32_t)(ag + (bgc - ag) * t / n) << 8) | (uint32_t)(ab + (bb - ab) * t / n);
}

static inline uint32_t shade(uint32_t c, int pct) {
    int r = (int)((c >> 16) & 255) * pct / 100;
    int g = (int)((c >> 8) & 255) * pct / 100;
    int b = (int)(c & 255) * pct / 100;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

struct Clock {
    int64_t last;
    Clock() : last(esp_timer_get_time()) {}
    float tick() {
        const int64_t n = esp_timer_get_time();
        float d = (float)(n - last) / 1000000.0f;
        last = n;
        if (d > 0.05f) d = 0.05f;
        if (d < 0.001f) d = 0.001f;
        return d;
    }
};

struct Canvas {
    M5Canvas cv;
    int W, H;
    bool ok;
    Canvas() : cv(&M5.Display), W(M5.Display.width()), H(M5.Display.height()), ok(false) {
        cv.setColorDepth(16);
        cv.setPsram(true);
        ok = (cv.createSprite(W, H) != nullptr);
    }
    void push() { cv.pushSprite(&M5.Display, 0, 0); }
};

static inline bool need(Canvas &c) {
    if (c.ok) return true;
    M5.Display.fillScreen(0x000000);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(0xFFFFFF, 0x000000);
    M5.Display.setCursor(8, M5.Display.height() / 2 - 8);
    M5.Display.print("No memory");
    vTaskDelay(pdMS_TO_TICKS(1500));
    return false;
}

struct Part { float x, y, vx, vy, life, max; uint32_t col; };

template <int N>
struct Parts {
    Part p[N];
    int next;
    Parts() : next(0) { memset(p, 0, sizeof(p)); }
    void burst(float x, float y, int n, uint32_t col, float spd, float life) {
        for (int i = 0; i < n; i++) {
            Part &q = p[next];
            next = (next + 1) % N;
            const float a = frand() * 6.2832f, s = spd * (0.3f + 0.7f * frand());
            q.x = x; q.y = y; q.vx = cosf(a) * s; q.vy = sinf(a) * s;
            q.life = life * (0.6f + 0.4f * frand());
            q.max = q.life;
            q.col = col;
        }
    }
    void update(float dt, float grav) {
        for (int i = 0; i < N; i++) {
            Part &q = p[i];
            if (q.life <= 0.0f) continue;
            q.life -= dt;
            q.vy += grav * dt;
            q.x += q.vx * dt;
            q.y += q.vy * dt;
        }
    }
    void draw(M5Canvas &cv) {
        for (int i = 0; i < N; i++) {
            const Part &q = p[i];
            if (q.life <= 0.0f) continue;
            const int s = (q.life > q.max * 0.5f) ? 2 : 1;
            cv.fillRect((int)q.x, (int)q.y, s, s, q.col);
        }
    }
};

static inline int text_w(const char *s, int size = 1) { return (int)strlen(s) * 6 * size; }

static inline void text(M5Canvas &cv, int x, int y, const char *s, uint32_t col, int size = 1) {
    cv.setTextSize(size);
    cv.setTextColor(0x000000);
    cv.setCursor(x + 1, y + 1);
    cv.print(s);
    cv.setTextColor(col);
    cv.setCursor(x, y);
    cv.print(s);
}

// end-of-game panel drawn over the last frame; true = retry (B), false = exit (A or A+B)
static inline bool end_screen(Canvas &c, const char *title, uint32_t title_col, int score, int best, bool newrec, const char *extra, const char *label = "Score") {
    M5Canvas &cv = c.cv;
    const int W = c.W, H = c.H;
    int pw = W - 8;
    if (pw > 160) pw = 160;
    const int ph = 82, x0 = (W - pw) / 2, y0 = (H - ph) / 2;
    cv.fillRoundRect(x0, y0, pw, ph, 6, 0x101828);
    cv.drawRoundRect(x0, y0, pw, ph, 6, 0x66CCFF);
    text(cv, W / 2 - text_w(title, 2) / 2, y0 + 6, title, title_col, 2);
    char b[40];
    snprintf(b, sizeof(b), "%s %d   Best %d", label, score, best);
    text(cv, W / 2 - text_w(b) / 2, y0 + 30, b, 0xFFFFFF);
    if (newrec) text(cv, W / 2 - text_w("NEW RECORD!") / 2, y0 + 43, "NEW RECORD!", 0xFFD54F);
    else if (extra && extra[0]) text(cv, W / 2 - text_w(extra) / 2, y0 + 43, extra, 0xA5D6A7);
    text(cv, W / 2 - text_w("B retry   A exit") / 2, y0 + 62, "B retry   A exit", 0x9CCBFF);
    c.push();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) { k85_wake_screen(); return true; }
        if (k85_btn_a_pressed()) { k85_wake_screen(); return false; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

}  // namespace gk