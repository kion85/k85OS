#include "system_info.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "sound.h"
#include "config.h"
#include "device.h"
#include "rtc_ntp.h"
#include "step_counter.h"

#include "M5Unified.h"
#include "esp_heap_caps.h"
#include "driver/temperature_sensor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

#define K85_FW_NAME "k85OS"
#define K85_FW_VERSION "v4.2"

static temperature_sensor_handle_t s_temp_handle = nullptr;
static bool s_temp_ready = false;

static bool init_temp_sensor(void) {
    if (s_temp_ready) return true;
    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    if (temperature_sensor_install(&temp_cfg, &s_temp_handle) != ESP_OK) return false;
    if (temperature_sensor_enable(s_temp_handle) != ESP_OK) return false;
    s_temp_ready = true;
    return true;
}

static bool get_cpu_temp_c(float *out) {
    if (!init_temp_sensor()) return false;
    return temperature_sensor_get_celsius(s_temp_handle, out) == ESP_OK;
}

// РђРЅР°Р»РѕРі run_system_info() РёР· MicroPython (+ РґРѕР±Р°РІР»РµРЅР° СЃС‚СЂРѕРєР° Storage: LittleFS,
// С‚.Рє. РѕС‚РґРµР»СЊРЅРѕРіРѕ Settings-СЌРєСЂР°РЅР° СЃ СЌС‚РѕР№ РёРЅС„РѕР№ РїРѕРєР° РЅРµС‚ вЂ” РїРѕСЏРІРёС‚СЃСЏ РІ СЃР»РѕРµ 6)
void k85_run_system_info(void) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    while (true) {
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);

        multi_heap_info_t heap_info;
        heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);
        unsigned used_kb = (unsigned)(heap_info.total_allocated_bytes / 1024);
        unsigned total_kb = (unsigned)((heap_info.total_allocated_bytes + heap_info.total_free_bytes) / 1024);

        size_t fs_total = 0, fs_used = 0;
        bool fs_ok = k85_fs_info(&fs_total, &fs_used);

        int batt = k85_get_battery();
        float temp_c = 0;
        bool has_temp = get_cpu_temp_c(&temp_c);

        int y = 6;
        char line[48];

        M5.Display.setCursor(10, y); M5.Display.print(K85_FW_NAME " " K85_FW_VERSION); y += 12;

        snprintf(line, sizeof(line), "Device: %s", k85_get_device_name());
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        snprintf(line, sizeof(line), "Theme: %s", k85_get_theme()->name);
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        int mode_idx = g_config.battery_mode_idx;
        snprintf(line, sizeof(line), "Mode: %s",
                 (mode_idx >= 0 && mode_idx < K85_BATTERY_MODE_COUNT) ? k85_battery_modes[mode_idx] : "?");
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        snprintf(line, sizeof(line), "Volume: %d%%", k85_get_sound_volume());
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        y += 12; // РїСѓСЃС‚Р°СЏ СЃС‚СЂРѕРєР°, РєР°Рє РІ РѕСЂРёРіРёРЅР°Р»Рµ

        snprintf(line, sizeof(line), "RAM: %u/%u KB", used_kb, total_kb);
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        if (fs_ok) {
            snprintf(line, sizeof(line), "Storage: LittleFS %u/%uKB",
                     (unsigned)(fs_used / 1024), (unsigned)(fs_total / 1024));
        } else {
            snprintf(line, sizeof(line), "Storage: LittleFS N/A");
        }
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        if (batt >= 0) {
            snprintf(line, sizeof(line), "Battery: %d%%%s", batt, k85_is_charging() ? " (chg)" : "");
        } else {
            snprintf(line, sizeof(line), "Battery: N/A");
        }
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        if (has_temp) {
            snprintf(line, sizeof(line), "CPU temp: %.1f C", temp_c);
        } else {
            snprintf(line, sizeof(line), "CPU temp: N/A");
        }
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        // TODO: СЃР»РѕР№ net/wifi Р·Р°РјРµРЅРёС‚ РЅР° СЂРµР°Р»СЊРЅС‹Р№ СЃС‚Р°С‚СѓСЃ РїРѕРґРєР»СЋС‡РµРЅРёСЏ
        M5.Display.setCursor(10, y); M5.Display.print("WiFi: N/A"); y += 12;

        snprintf(line, sizeof(line), "NTP: %s", k85_is_ntp_synced() ? "synced" : "no");
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        snprintf(line, sizeof(line), "Steps: %d", k85_get_step_count());
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        snprintf(line, sizeof(line), "Time: %s", k85_get_time_str());
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        snprintf(line, sizeof(line), "Date: %s", k85_get_date_str());
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        snprintf(line, sizeof(line), "Uptime: %s", k85_get_uptime_str());
        M5.Display.setCursor(10, y); M5.Display.print(line); y += 12;

        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(10, M5.Display.height() - 12);
        M5.Display.print("A+B=back (updates live)");

        k85_draw_battery_icon();

        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
