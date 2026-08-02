#include "lock_screen.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "config.h"
#include "rtc_ntp.h"
#include "device.h"
#include "step_counter.h"
#include "M5Unified.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>

#define K85_LOCK_PARTICLES 20

struct LockParticle {
    float x, y, vx, vy;
    int size;
    uint32_t color;
};

static LockParticle s_particles[K85_LOCK_PARTICLES];
static bool s_particles_init = false;

static float rand_offset(float scale) {
    return ((float)(esp_random() % 1000) / 1000.0f - 0.5f) * scale;
}

static void init_particles() {
    if (s_particles_init) return;
    static const uint32_t colors[] = {0x00FFFF, 0xFF00FF, 0xFFFF00, 0x00FF00, 0xFF6600};
    int w = M5.Display.width();
    int h = M5.Display.height();
    for (int i = 0; i < K85_LOCK_PARTICLES; i++) {
        s_particles[i].x = (float)(esp_random() % w);
        s_particles[i].y = (float)(esp_random() % h);
        s_particles[i].vx = rand_offset(10.0f);
        s_particles[i].vy = rand_offset(10.0f);
        s_particles[i].size = (int)(esp_random() % 3) + 1;
        s_particles[i].color = colors[esp_random() % 5];
    }
    s_particles_init = true;
}

// Аналог draw_lock_screen()
static void draw_lock_screen() {
    int mode_idx = g_config.battery_mode_idx;
    const char *mode = (mode_idx >= 0 && mode_idx < K85_BATTERY_MODE_COUNT)
                            ? k85_battery_modes[mode_idx] : "Balanced";
    int w = M5.Display.width();
    int h = M5.Display.height();

    if (!strcmp(mode, "SuperEco")) {
        init_particles();
        M5.Display.fillScreen(0x000000);
        for (int i = 0; i < K85_LOCK_PARTICLES; i++) {
            LockParticle &p = s_particles[i];
            p.x += p.vx * 0.1f;
            p.y += p.vy * 0.1f;
            p.vy += 0.05f;
            if (p.x < 0 || p.x > w) { p.vx *= -0.8f; if (p.x < 0) p.x = 0; if (p.x > w) p.x = (float)w; }
            if (p.y < 0 || p.y > h) { p.vy *= -0.8f; if (p.y < 0) p.y = 0; if (p.y > h) p.y = (float)h; }
            M5.Display.fillCircle((int)p.x, (int)p.y, p.size, p.color);
        }
    } else {
        M5.Display.fillScreen(0x000000);
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
    while (true) {
        k85_input_update();
        k85_step_counter_update();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_redraw > 500) {
            draw_lock_screen();
            last_redraw = now;
        }
        if (k85_ab_held(600)) {
            k85_wait_ab_release();
            s_particles_init = false;
            k85_wake_screen();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}