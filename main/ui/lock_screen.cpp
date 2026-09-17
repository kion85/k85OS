#include "lock_screen.h"
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

static void init_screensaver_letters() {
    if (s_ss_letters_init) return;
    const char *text = "k85os";
    int w = M5.Display.width();
    int h = M5.Display.height();
    for (int i = 0; i < K85_SS_LETTER_COUNT; i++) {
        s_ss_letters[i].ch = text[i];
        s_ss_letters[i].x = (float)(esp_random() % (w > 24 ? w - 24 : 1));
        s_ss_letters[i].y = (float)(esp_random() % (h > 28 ? h - 28 : 1));
        s_ss_letters[i].vx = rand_offset(4.0f);
        s_ss_letters[i].vy = rand_offset(4.0f);
        if (s_ss_letters[i].vx > -0.5f && s_ss_letters[i].vx < 0.5f) s_ss_letters[i].vx = 1.2f;
        if (s_ss_letters[i].vy > -0.5f && s_ss_letters[i].vy < 0.5f) s_ss_letters[i].vy = 1.2f;
    }
    s_ss_letters_init = true;
}

static void draw_screensaver() {
    int w = M5.Display.width();
    int h = M5.Display.height();
    init_screensaver_letters();
    M5.Display.fillScreen(0x000000);

    static const uint32_t colors[K85_SS_LETTER_COUNT] = {0x00FFFF, 0xFF66FF, 0xFFFF66, 0x66FF66, 0xFF9966};
    M5.Display.setTextSize(3);
    for (int i = 0; i < K85_SS_LETTER_COUNT; i++) {
        ScreensaverLetter &l = s_ss_letters[i];
        l.x += l.vx;
        l.y += l.vy;
        if (l.x < 0 || l.x > w - 20) { l.vx = -l.vx; if (l.x < 0) l.x = 0; if (l.x > w - 20) l.x = (float)(w - 20); }
        if (l.y < 0 || l.y > h - 24) { l.vy = -l.vy; if (l.y < 0) l.y = 0; if (l.y > h - 24) l.y = (float)(h - 24); }

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

    while (true) {
        k85_input_update();
        k85_step_counter_update();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        bool any_input = k85_btn_a_is_down() || k85_btn_b_is_down();
        if (any_input) {
            idle_start_ms = now;
            if (screensaver_on) {
                screensaver_on = false;
                s_ss_letters_init = false;
                k85_speaker_stop();
                M5.Speaker.setVolume(ss_saved_volume);
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
        }

        if (screensaver_on) {
            if (now - last_redraw > 60) {
                draw_screensaver();
                last_redraw = now;
            }
            if (now - ss_last_note_ms >= K85_SS_NOTE_MS) {
                k85_play_tone(K85_SS_NOTES[ss_note_idx], K85_SS_NOTE_MS);
                ss_note_idx = (ss_note_idx + 1) % K85_SS_NOTE_COUNT;
                ss_last_note_ms = now;
            }
        } else if (now - last_redraw > 500) {
            draw_lock_screen();
            last_redraw = now;
        }

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



