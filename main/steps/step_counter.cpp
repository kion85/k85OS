#include "step_counter.h"
#include "config.h"
#include "rtc_ntp.h"
#include "M5Unified.h"
#include "esp_timer.h"

#include <cstring>
#include <cstdio>

static uint32_t s_last_check_ms = 0;
static float s_prev_az = 0.0f;
static const float K85_STEP_THRESHOLD = 1.2f;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void k85_step_counter_update(void) {
    uint32_t now = now_ms();
    if (now - s_last_check_ms < 80) return;
    s_last_check_ms = now;

    float ax = 0, ay = 0, az = 0;
    M5.Imu.getAccel(&ax, &ay, &az);

    float dz = az - s_prev_az;
    s_prev_az = az;

    if (dz > K85_STEP_THRESHOLD && az > 0.5f) {
        g_config.step_count++;
        const char *today = k85_get_date_str();
        if (strcmp(g_config.step_date, today) != 0) {
            snprintf(g_config.step_date, sizeof(g_config.step_date), "%s", today);
            g_config.step_record = g_config.step_count;
        } else if (g_config.step_count > g_config.step_record) {
            g_config.step_record = g_config.step_count;
        }
        k85_config_save();
    }
}

int k85_get_step_count(void) { return g_config.step_count; }

void k85_reset_step_counter(void) {
    g_config.step_count = 0;
    snprintf(g_config.step_date, sizeof(g_config.step_date), "%s", k85_get_date_str());
    g_config.step_record = 0;
    k85_config_save();
}