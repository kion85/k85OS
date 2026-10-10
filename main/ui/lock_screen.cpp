#include "lock_screen.h"
#include "voice.h" // K85V:inc
#include <cstdio>
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "config.h"
#include "rtc_ntp.h"
#include "device.h"
#include "step_counter.h"
#include "text_input.h"
#include "../core/lock_auth.h"
#include "sound.h"
#include "wifi.h"
#include <cstring>
#include "M5Unified.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <cmath>
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>

#define K85_LOCK_PARTICLES 14
#define K85_LOCK_STARS 25

struct LockParticle {
    float x, y, vx, vy;
    int size;
    uint32_t color;
};

struct LockStar {
    int16_t x, y;
    uint8_t phase;
};

static LockParticle s_particles[K85_LOCK_PARTICLES];
static LockStar s_stars[K85_LOCK_STARS];
static bool s_particles_init = false;

static float rand_offset(float scale) {
    return ((float)(esp_random() % 1000) / 1000.0f - 0.5f) * scale;
}

static void init_particles() {
    if (s_particles_init) return;
    static const uint32_t colors[] = {0x88CCFF, 0xFFFFFF, 0xCC99FF, 0x99FFEE, 0xFFDD99};
    int w = M5.Display.width();
    int h = M5.Display.height();
    for (int i = 0; i < K85_LOCK_PARTICLES; i++) {
        s_particles[i].x = (float)(esp_random() % w);
        s_particles[i].y = (float)(esp_random() % h);
        s_particles[i].vx = rand_offset(6.0f);
        s_particles[i].vy = rand_offset(6.0f);
        s_particles[i].size = (int)(esp_random() % 2) + 1;
        s_particles[i].color = colors[esp_random() % 5];
    }
    for (int i = 0; i < K85_LOCK_STARS; i++) {
        s_stars[i].x = (int16_t)(esp_random() % w);
        s_stars[i].y = (int16_t)(esp_random() % h);
        s_stars[i].phase = (uint8_t)(esp_random() % 255);
    }
    s_particles_init = true;
}

// ---------- Скринсейвер: "k85os" летающими буквами + тихая мелодия ----------
// Появляется после K85_LOCK_SCREENSAVER_IDLE_MS простоя на экране блокировки,
// но ТОЛЬКО если WiFi не подключён (иначе вообще не показывается).
#define K85_LOCK_SCREENSAVER_IDLE_MS 85000
#define K85_SS_LETTER_COUNT 5
#define K85_SS_NOTE_MS 350

struct ScreensaverLetter {
    float x, y, vx, vy;
    char ch;
};

static ScreensaverLetter s_ss_letters[K85_SS_LETTER_COUNT];
static bool s_ss_letters_init = false;

static const uint32_t K85_SS_NOTES[] = {523, 659, 784, 659, 523, 392, 523};
#define K85_SS_NOTE_COUNT (int)(sizeof(K85_SS_NOTES) / sizeof(K85_SS_NOTES[0]))

// K85V:ss ---- calm screensaver: soft bell melody + kind words on top ----
static const char *const K85_SS_PHRASES[] = {
    "You are a good person", "Everything will be okay", "You can do it",
    "Be kind to yourself", "Small steps still count", "You are doing great",
    "This too shall pass", "Breathe. You got this", "Today is a good day", "You matter",
    "You are stronger than you think", "Rest is not a waste of time", "You are enough, just as you are",
    "Good things are coming", "One step at a time",
};
#define K85_SS_PHRASE_COUNT (int)(sizeof(K85_SS_PHRASES) / sizeof(K85_SS_PHRASES[0]))
#define K85_SS_PHRASE_MS 7000
static int s_ss_phrase_idx = 0;
static uint32_t s_ss_phrase_start = 0;

static int ss_text_size() { return M5.Display.width() >= 200 ? 2 : 1; }
static int ss_top() { return 2 * (8 * ss_text_size() + 4) + 10; }   // letters stay below the message

static uint32_t ss_scale(uint32_t col, int b) {   // b = 0..255
    uint32_t r = ((col >> 16) & 0xFF) * (uint32_t)b / 255;
    uint32_t g = ((col >> 8) & 0xFF) * (uint32_t)b / 255;
    uint32_t bl = (col & 0xFF) * (uint32_t)b / 255;
    return (r << 16) | (g << 8) | bl;
}

static void ss_draw_message() {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t el = now - s_ss_phrase_start;
    if (el >= K85_SS_PHRASE_MS) {
        s_ss_phrase_start = now;
        el = 0;
        s_ss_phrase_idx = (s_ss_phrase_idx + 1) % K85_SS_PHRASE_COUNT;
    }
    int b;
    if (el < 1200) b = (int)(el * 255 / 1200);
    else if (el > K85_SS_PHRASE_MS - 1200) b = (int)((K85_SS_PHRASE_MS - el) * 255 / 1200);
    else b = 255;

    int w = M5.Display.width();
    int sz = ss_text_size();
    int cw = 6 * sz;
    int max_chars = (w - 8) / cw;
    if (max_chars < 6) max_chars = 6;

    const char *p = K85_SS_PHRASES[s_ss_phrase_idx];
    char lines[2][40] = {{0}, {0}};
    int ln = 0, len = (int)strlen(p), pos = 0;
    while (pos < len && ln < 2) {
        int take = len - pos;
        if (take > max_chars) {
            take = max_chars;
            int sp = take;
            while (sp > 0 && p[pos + sp] != ' ') sp--;
            if (sp > 0) take = sp;
        }
        if (take > 39) take = 39;
        memcpy(lines[ln], p + pos, (size_t)take);
        lines[ln][take] = 0;
        pos += take;
        while (pos < len && p[pos] == ' ') pos++;
        ln++;
    }

    M5.Display.setTextSize(sz);
    M5.Display.setTextColor(ss_scale(0xFFDD99, b), 0x000000);
    for (int i = 0; i < ln; i++) {
        int x = (w - (int)strlen(lines[i]) * cw) / 2;
        if (x < 0) x = 0;
        M5.Display.setCursor(x, 4 + i * (8 * sz + 4));
        M5.Display.print(lines[i]);
    }
}

// a small lullaby in C major pentatonic: {frequency Hz, wait before next note ms}
struct SsNote { uint16_t hz; uint16_t wait_ms; };
static const SsNote K85_SS_MELODY[] = {
    {659, 480}, {784, 480}, {880, 720}, {784, 480}, {659, 480}, {587, 720}, {523, 1500},
    {587, 480}, {659, 480}, {784, 720}, {659, 480}, {587, 480}, {523, 960}, {440, 1800},
    {659, 480}, {784, 480}, {1047, 960}, {880, 600}, {784, 600}, {659, 720}, {784, 480}, {659, 480}, {587, 720}, {523, 2400},
};
#define K85_SS_MELODY_COUNT (int)(sizeof(K85_SS_MELODY) / sizeof(K85_SS_MELODY[0]))

#define K85_SS_RATE 16000
#define K85_SS_BELL_SAMPLES 11000   // ~0.7 s per bell  // K85V:fast
#define K85_SS_CACHE 10
struct SsBell { uint16_t hz; int16_t *buf; };
static SsBell s_ss_cache[K85_SS_CACHE] = {};

static void ss_bell_free() {
    M5.Speaker.stop();
    vTaskDelay(pdMS_TO_TICKS(40));
    for (int i = 0; i < K85_SS_CACHE; i++) {
        if (s_ss_cache[i].buf) heap_caps_free(s_ss_cache[i].buf);
        s_ss_cache[i].buf = nullptr;
        s_ss_cache[i].hz = 0;
    }
}

// Each pitch is rendered ONCE (recurrence oscillators: no sinf per sample, ~3 ms)
// and then reused by every later note, so the screensaver animation never stalls.
static int16_t *ss_get_bell(uint32_t hz) {
    for (int i = 0; i < K85_SS_CACHE; i++)
        if (s_ss_cache[i].buf && s_ss_cache[i].hz == hz) return s_ss_cache[i].buf;
    int slot = -1;
    for (int i = 0; i < K85_SS_CACHE; i++) if (!s_ss_cache[i].buf) { slot = i; break; }
    if (slot < 0) return nullptr;
    int16_t *buf = (int16_t *)heap_caps_malloc(K85_SS_BELL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return nullptr;

    const float w0 = 2.0f * 3.14159265f * (float)hz / (float)K85_SS_RATE;
    const float c1 = 2.0f * cosf(w0), c2 = 2.0f * cosf(2.0f * w0), c3 = 2.0f * cosf(3.0f * w0);
    float p1 = -sinf(w0), p2 = -sinf(2.0f * w0), p3 = -sinf(3.0f * w0);   // y[-1]
    float y1 = 0.0f, y2 = 0.0f, y3 = 0.0f;                                  // y[0]
    const float decay = expf(-4.2f / (float)K85_SS_RATE);
    const float decay2 = expf(-8.0f / (float)K85_SS_RATE);
    float env = 1.0f, env2 = 1.0f;
    for (int i = 0; i < K85_SS_BELL_SAMPLES; i++) {
        float att = i < 160 ? (float)i / 160.0f : 1.0f;
        float s = y1 * env + 0.30f * y2 * env2 + 0.10f * y3 * env2 * env2;
        buf[i] = (int16_t)(s * att * 9000.0f);
        float n1 = c1 * y1 - p1; p1 = y1; y1 = n1;
        float n2 = c2 * y2 - p2; p2 = y2; y2 = n2;
        float n3 = c3 * y3 - p3; p3 = y3; y3 = n3;
        env *= decay;
        env2 *= decay2;
    }
    s_ss_cache[slot].hz = (uint16_t)hz;
    s_ss_cache[slot].buf = buf;
    return buf;
}

static void ss_bell(uint32_t hz) {
    if (hz == 0 || M5.Mic.isRunning()) return;
    int16_t *buf = ss_get_bell(hz);
    if (!buf) return;
    M5.Speaker.playRaw(buf, K85_SS_BELL_SAMPLES, K85_SS_RATE, false, 1, -1, false);
}
static void init_screensaver_letters() {
    if (s_ss_letters_init) return;
    const char *text = "k85os";
    int w = M5.Display.width();
    int h = M5.Display.height();
    for (int i = 0; i < K85_SS_LETTER_COUNT; i++) {
        s_ss_letters[i].ch = text[i];
        s_ss_letters[i].x = (float)(esp_random() % (w > 24 ? w - 24 : 1));
        s_ss_letters[i].y = (float)ss_top() + (float)(esp_random() % ((h - 24 - ss_top()) > 1 ? (unsigned)(h - 24 - ss_top()) : 1u));
        s_ss_letters[i].vx = rand_offset(4.0f);
        s_ss_letters[i].vy = rand_offset(4.0f);
        if (s_ss_letters[i].vx > -0.5f && s_ss_letters[i].vx < 0.5f) s_ss_letters[i].vx = 1.2f;
        if (s_ss_letters[i].vy > -0.5f && s_ss_letters[i].vy < 0.5f) s_ss_letters[i].vy = 1.2f;
    }
    s_ss_letters_init = true;
    s_ss_phrase_idx = (int)(esp_random() % K85_SS_PHRASE_COUNT);
    s_ss_phrase_start = (uint32_t)(esp_timer_get_time() / 1000);
}

static void draw_screensaver() {
    int w = M5.Display.width();
    int h = M5.Display.height();
    init_screensaver_letters();
    M5.Display.fillScreen(0x000000);
    ss_draw_message();

    static const uint32_t colors[K85_SS_LETTER_COUNT] = {0x00FFFF, 0xFF66FF, 0xFFFF66, 0x66FF66, 0xFF9966};
    M5.Display.setTextSize(3);
    for (int i = 0; i < K85_SS_LETTER_COUNT; i++) {
        ScreensaverLetter &l = s_ss_letters[i];
        l.x += l.vx;
        l.y += l.vy;
        if (l.x < 0 || l.x > w - 20) { l.vx = -l.vx; if (l.x < 0) l.x = 0; if (l.x > w - 20) l.x = (float)(w - 20); }
        if (l.y < (float)ss_top() || l.y > h - 24) { l.vy = -l.vy; if (l.y < (float)ss_top()) l.y = (float)ss_top(); if (l.y > h - 24) l.y = (float)(h - 24); }

        char buf[2] = { l.ch, 0 };
        M5.Display.setTextColor(colors[i], 0x000000);
        M5.Display.setCursor((int)l.x, (int)l.y);
        M5.Display.print(buf);
    }
}

static void draw_lock_screen() {
    int w = M5.Display.width();
    int h = M5.Display.height();

    init_particles();
    M5.Display.fillScreen(0x000000);

    for (int i = 0; i < K85_LOCK_STARS; i++) {
        LockStar &s = s_stars[i];
        s.phase += 3;
        int brightness = s.phase < 128 ? s.phase * 2 : (255 - s.phase) * 2;
        if (brightness < 40) brightness = 40;
        uint8_t v = (uint8_t)brightness;
        uint32_t col = ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        M5.Display.drawPixel(s.x, s.y, col);
    }

    for (int i = 0; i < K85_LOCK_PARTICLES; i++) {
        LockParticle &p = s_particles[i];
        p.x += p.vx * 0.1f;
        p.y += p.vy * 0.1f;
        p.vy += 0.05f;
        if (p.x < 0 || p.x > w) { p.vx *= -0.8f; if (p.x < 0) p.x = 0; if (p.x > w) p.x = (float)w; }
        if (p.y < 0 || p.y > h) { p.vy *= -0.8f; if (p.y < 0) p.y = 0; if (p.y > h) p.y = (float)h; }

        uint32_t draw_color = (g_config.lock_particle_color == 0xFFFFFFFF) ? p.color : g_config.lock_particle_color;
        int shape = g_config.lock_shape;
        bool draw_square = (shape == 1) || (shape == 2 && (i % 2 == 1));
        if (draw_square) {
            M5.Display.fillRect((int)p.x - p.size, (int)p.y - p.size, p.size * 2, p.size * 2, draw_color);
        } else {
            M5.Display.fillCircle((int)p.x, (int)p.y, p.size, draw_color);
        }
    }

    const char *t = k85_get_time_str();
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(0xFFFFFF, 0x000000);
    int approx_w = (int)strlen(t) * 18;
    M5.Display.setCursor((w - approx_w) / 2, h / 2 - 35);
    M5.Display.print(t);

    if (k85_is_ntp_synced()) {
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0x888888, 0x000000);
        const char *d = k85_get_date_str();
        M5.Display.setCursor((w - (int)strlen(d) * 6) / 2, h / 2 - 18);
        M5.Display.print(d);
    }

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x888888, 0x000000);
    const char *name = k85_get_device_name();
    M5.Display.setCursor((w - (int)strlen(name) * 6) / 2, h / 2 + 5);
    M5.Display.print(name);

    int batt = k85_get_battery();
    int steps = k85_get_step_count();
    char info[32];
    if (batt >= 0) {
        if (steps > 0) snprintf(info, sizeof(info), "%d%% | steps:%d", batt, steps);
        else snprintf(info, sizeof(info), "%d%%", batt);
    } else {
        snprintf(info, sizeof(info), "--%%");
    }
    M5.Display.setTextColor(0xAAAAAA, 0x000000);
    M5.Display.setCursor((w - (int)strlen(info) * 6) / 2, h / 2 + 18);
    M5.Display.print(info);

    M5.Display.setTextColor(0x555555, 0x000000);
    M5.Display.setCursor(10, h - 14);
    M5.Display.print("A+B hold to unlock");
}

void k85_lock_screen_loop(void) {
    int mode_idx = g_config.battery_mode_idx;
    const char *mode = (mode_idx >= 0 && mode_idx < K85_BATTERY_MODE_COUNT)
                            ? k85_battery_modes[mode_idx] : "Balanced";

    if (!strcmp(mode, "SuperEco")) {
        M5.Display.setBrightness(K85_BRIGHTNESS_ECO);
    } else {
        int b = g_config.brightness_active / 3;
        if (b < K85_BRIGHTNESS_IDLE) b = K85_BRIGHTNESS_IDLE;
        M5.Display.setBrightness(b);
    }

    uint32_t last_redraw = 0;
    uint32_t idle_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool screensaver_on = false;
    uint8_t ss_saved_volume = 0;
    int ss_note_idx = 0;
    uint32_t ss_last_note_ms = 0;
    uint32_t ss_next_wait = 0;
    uint32_t k85v_a_start = 0; bool k85v_a_fired = false; // K85V:decl
    k85_voice_ensure_pack();

    while (true) {
        k85_input_update();
        k85_step_counter_update();
        if (k85_voice_take_notify()) { k85_voice_flash_ready(); last_redraw = 0; }  // K85V:notify
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        bool any_input = k85_btn_a_is_down() || k85_btn_b_is_down();
        if (any_input) {
            idle_start_ms = now;
            if (screensaver_on) {
                screensaver_on = false;
                s_ss_letters_init = false;
                k85_speaker_stop();
                M5.Speaker.setVolume(ss_saved_volume);
                ss_bell_free();
                last_redraw = 0;
            }
        }

        if (!screensaver_on &&
            (now - idle_start_ms) >= K85_LOCK_SCREENSAVER_IDLE_MS) {
            screensaver_on = true;
            ss_saved_volume = M5.Speaker.getVolume();
            M5.Speaker.setVolume(127); // ~50%
            ss_note_idx = 0;
            ss_last_note_ms = 0;
            ss_next_wait = 0;
        }

        if (screensaver_on) {
            if (now - last_redraw > 60) {
                draw_screensaver();
                last_redraw = now;
            }
            if (now - ss_last_note_ms >= ss_next_wait) {
                ss_bell(K85_SS_MELODY[ss_note_idx].hz);
                ss_next_wait = K85_SS_MELODY[ss_note_idx].wait_ms;
                ss_note_idx = (ss_note_idx + 1) % K85_SS_MELODY_COUNT;
                ss_last_note_ms = now;
            }
        } else if (now - last_redraw > 500) {
            draw_lock_screen();
            last_redraw = now;
        }

        // K85V:gest - удержание только A 2 с включает/выключает озвучку
        if (!screensaver_on && k85_btn_a_is_down() && !k85_btn_b_is_down()) {
            if (k85v_a_start == 0) k85v_a_start = now;
            else if (!k85v_a_fired && now - k85v_a_start >= 2000) {
                k85v_a_fired = true;
                if (!k85_voice_enabled()) { g_config.sound_muted = false; g_config.sound_volume = 55; } // K85V:unmute
        k85_voice_set(!k85_voice_enabled());
            }
        } else { k85v_a_start = 0; k85v_a_fired = false; }
        if (!screensaver_on && k85_ab_held(600)) {
            k85_wait_ab_release();
            if (g_config.lock_enabled && g_config.lock_password[0]) {
                char entered[32] = "";
                bool submitted = k85_text_input("Enter password:", "", entered, sizeof(entered));

                if (submitted && k85_lock_verify_password(entered, g_config.lock_password)) {
                    s_particles_init = false;
                    k85_wake_screen();
                    return;
                }
                // неверный пароль или отмена — остаёмся заблокированы, экран перерисуется на следующей итерации
                continue;
            }
            s_particles_init = false;
            k85_wake_screen();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}



