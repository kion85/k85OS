#include "power.h"
#include "config.h"
#include "battery.h"
#include "notifications.h"
#include "M5Unified.h"
#include "esp_timer.h"

const char *k85_battery_modes[K85_BATTERY_MODE_COUNT] = {
    "Normal", "Balanced", "SuperEco"
};

// BATTERY_IDLE_TIMEOUTS = {"Normal": 20000, "Balanced": 8000, "SuperEco": 3000}
static const uint32_t k85_idle_timeouts_ms[K85_BATTERY_MODE_COUNT] = {
    20000, 8000, 3000
};

static uint32_t s_last_activity = 0;
static bool s_dimmed = false;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void k85_power_init(void) {
    s_last_activity = now_ms();
    s_dimmed = false;
}

uint32_t k85_idle_timeout_ms(void) {
    int idx = g_config.battery_mode_idx;
    if (idx < 0 || idx >= K85_BATTERY_MODE_COUNT) idx = 1;
    return k85_idle_timeouts_ms[idx];
}

static bool s_notified_15 = false;
static bool s_notified_5 = false;

static void check_battery_notify(void) {
    int batt = k85_get_battery();
    if (batt < 0) return;
    if (batt <= 5 && !s_notified_5) {
        k85_notify("Battery low: %d%%", batt);
        s_notified_5 = true;
        s_notified_15 = true;
    } else if (batt <= 15 && !s_notified_15) {
        k85_notify("Battery: %d%%", batt);
        s_notified_15 = true;
    } else if (batt > 20) {
        s_notified_15 = false;
        s_notified_5 = false;
    }
}

bool k85_power_tick(void) {
    check_battery_notify();
    if (s_dimmed) return false;
    uint32_t elapsed = now_ms() - s_last_activity;
    if (elapsed > k85_idle_timeout_ms()) {
        s_dimmed = true;
        M5.Display.setBrightness(K85_BRIGHTNESS_IDLE);
        return true;
    }
    return false;
}

void k85_wake_screen(void) {
    s_last_activity = now_ms();
    if (s_dimmed) {
        s_dimmed = false;
        M5.Display.setBrightness(g_config.brightness_active);
    }
}

bool k85_is_dimmed(void) { return s_dimmed; }

