#include "status_bar.h"
#include "../ui/look.h"
#include "config.h"
#include "theme.h"
#include "battery.h"
#include "rtc_ntp.h"
#include "sound.h"
#include "../net/wifi.h"
#include "../steps/step_counter.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/temperature_sensor.h"

#include <cstdio>
#include <cstring>

static temperature_sensor_handle_t s_temp_handle = nullptr;
static bool s_temp_ready = false;

static bool get_cpu_temp_c(float *out) {
    if (!s_temp_ready) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        if (temperature_sensor_install(&cfg, &s_temp_handle) != ESP_OK) return false;
        if (temperature_sensor_enable(s_temp_handle) != ESP_OK) return false;
        s_temp_ready = true;
    }
    return temperature_sensor_get_celsius(s_temp_handle, out) == ESP_OK;
}

// K85SB1: the text is rebuilt once per second and cached, so any screen may redraw the bar as often as it wants.
// Returns the x position where the text starts (the screen width when the bar is empty), so a title can be clipped.
static bool s_sb_overlay = false;   // K85SB3: overlay mode paints only a patch behind its own text
int k85_status_bar_draw(void) {
    uint32_t flags = g_config.sb_flags;
    uint32_t bg = (g_config.sb_bg_color == 0xFFFFFFFF) ? k85_get_bg() : g_config.sb_bg_color;
    uint32_t fg = k85_get_fg();

    int w = M5.Display.width();

    if (!s_sb_overlay) { if (!k85_banner_draw(0, w)) M5.Display.fillRect(0, 0, w, 12, bg); }   // K85LOOK1 // always clear the strip, otherwise old text leaves tails

    static char line[160];
    static int64_t built_us = -2000000;
    static uint32_t built_flags = 0xFFFFFFFFu;
    int64_t now_us = esp_timer_get_time();
    if (now_us - built_us >= 1000000 || flags != built_flags) {
        built_us = now_us;
        built_flags = flags;
        size_t used = 0;
        line[0] = 0;

        auto append = [&](const char *fmt, ...) {
            if (used >= sizeof(line) - 1) return;
            va_list args;
            va_start(args, fmt);
            int n = vsnprintf(line + used, sizeof(line) - used, fmt, args);
            va_end(args);
            if (n > 0) used += (size_t)n;
        };

        int batt = k85_get_battery();
        if (batt >= 0 && (flags & SB_BIT_BATTERY_PCT)) {
            append("%s%d%%", used ? " " : "", batt);
        }
        if (flags & SB_BIT_SOUND) {
            append("%sVol:%d%%", used ? " " : "", g_config.sound_muted ? 0 : k85_get_sound_volume());
        }
        if (flags & SB_BIT_TIME) {
            append("%s%s", used ? " " : "", k85_get_time_str());
        }
        if (flags & SB_BIT_DATE) {
            if (k85_is_ntp_synced()) append("%s%s", used ? " " : "", k85_get_date_str());
        }
        if (flags & SB_BIT_UPTIME) {
            append("%s%s", used ? " " : "", k85_get_uptime_str());
        }
        if (flags & SB_BIT_WIFI) {
            append("%sWiFi:%s", used ? " " : "", k85_wifi_is_connected() ? "On" : "Off");
        }
        if (flags & SB_BIT_BLUETOOTH) {
            append("%sBT:%s", used ? " " : "", g_config.bt_disabled ? "Off" : "Rdy");
        }
        if (flags & SB_BIT_STEPS) {
            append("%sSteps:%d", used ? " " : "", k85_get_step_count());
        }
        if (flags & SB_BIT_RAM) {
            multi_heap_info_t info;
            heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
            unsigned free_pct = (unsigned)((info.total_free_bytes * 100) /
                                 (info.total_free_bytes + info.total_allocated_bytes + 1));
            append("%sRAM:%u%%", used ? " " : "", free_pct);
        }
        if (flags & SB_BIT_TEMP) {
            float t;
            if (get_cpu_temp_c(&t)) append("%s%.0fC", used ? " " : "", t);
        }
    }

    M5.Display.setTextSize(1);
    if (k85_banner_active()) M5.Display.setTextColor(fg); else M5.Display.setTextColor(fg, bg);

    int text_w = (int)strlen(line) * 6;
    int x = w - text_w - 4;
    if (x < 0) x = 0;
    if (s_sb_overlay && line[0]) { if (!k85_banner_draw(x - 2, w - x + 2)) M5.Display.fillRect(x - 2, 0, w - x + 2, 12, bg); }
    M5.Display.setCursor(x, 4);
    M5.Display.print(line);
    return line[0] ? x : w;
}

// K85SB3: for screens that draw their own UI (apps, games, tools): the same bar and the same settings,
// but only the patch behind its own text is painted; the rest of the top strip stays with the screen.
void k85_status_bar_overlay(void) {
    s_sb_overlay = true;
    k85_status_bar_draw();
    s_sb_overlay = false;
}