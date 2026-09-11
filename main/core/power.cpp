#include "power.h"
#include "config.h"
#include "battery.h"
#include "notifications.h"
#include "input.h"
#include "log.h"
#include "M5Unified.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

const char *k85_battery_modes[K85_BATTERY_MODE_COUNT] = {
    "Normal", "Balanced", "SuperEco"
};
const char *k85_sleep_wake_mode_names[3] = {
    "Button A", "IMU", "Both"
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

// ---------- Sleep / Power Off / Reboot ----------

void k85_enter_sleep(void) {
    k85_log("Entering sleep (wake mode: %s)",
            k85_sleep_wake_mode_names[g_config.sleep_wake_mode >= 0 && g_config.sleep_wake_mode <= 2
                                       ? g_config.sleep_wake_mode : 0]);

    int saved_brightness = g_config.brightness_active;
    M5.Display.setBrightness(0);

    // WiFi в modem-sleep, а не полное отключение - если запущены SSH-сервер
    // или хотспот, соединение не рвётся, просто устройство меньше жрёт на
    // приёме. Если WiFi не был инициализирован - вызов просто вернёт ошибку,
    // это безопасно игнорируется.
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

    float base_ax = 0, base_ay = 0, base_az = 0;
    M5.Imu.getAccel(&base_ax, &base_ay, &base_az);

    int mode = g_config.sleep_wake_mode;
    if (mode < 0 || mode > 2) mode = K85_SLEEP_WAKE_BUTTON_A;

    while (true) {
        k85_input_update();
        bool wake = false;

        if (mode == K85_SLEEP_WAKE_BUTTON_A || mode == K85_SLEEP_WAKE_BOTH) {
            if (k85_btn_a_pressed()) wake = true;
        }
        if (!wake && (mode == K85_SLEEP_WAKE_IMU || mode == K85_SLEEP_WAKE_BOTH)) {
            float ax = 0, ay = 0, az = 0;
            M5.Imu.getAccel(&ax, &ay, &az);
            float delta = fabsf(ax - base_ax) + fabsf(ay - base_ay) + fabsf(az - base_az);
            if (delta > K85_SLEEP_IMU_THRESHOLD) wake = true;
        }
        if (wake) break;

        // Редкий опрос - экономим энергию, пока спим (обычный цикл в
        // остальном UI - 30мс, тут намеренно реже).
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    esp_wifi_set_ps(WIFI_PS_NONE);
    M5.Display.setBrightness(saved_brightness);
    k85_wake_screen();
    k85_log("Woke from sleep");
}

void k85_power_off(void) {
    k85_log("Power off requested");
    M5.Display.setBrightness(0);
    M5.Display.sleep();
    vTaskDelay(pdMS_TO_TICKS(100));
    // Без источников пробуждения - устройство остаётся в глубоком сне до
    // физического сброса/переподключения питания. Это и есть "выключено"
    // на платах без отдельной схемы отключения VBAT.
    esp_deep_sleep_start();
}

void k85_power_reboot(void) {
    k85_log("Reboot requested");
    vTaskDelay(pdMS_TO_TICKS(100)); // дать логу дойти до флеша/консоли
    esp_restart();
}