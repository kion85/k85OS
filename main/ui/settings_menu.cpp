#include "settings_menu.h"
#include "config.h"
#include "theme.h"
#include "power.h"
#include "sound.h"
#include "input.h"
#include "device.h"
#include "step_counter.h"
#include "wifi.h"
#include "common.h"
#include "boot_screen.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstddef>

#define K85_SETTINGS_ITEM_COUNT 9
#define K85_SETTINGS_BACK_IDX   (K85_SETTINGS_ITEM_COUNT - 1)

static const char *k85_settings_labels[K85_SETTINGS_ITEM_COUNT] = {
    "Theme", "Brightness", "Battery mode", "Boot style",
    "Device name", "Sound volume", "WiFi", "Reset steps",
    "Back",
};

static int s_selected = 0;

static void settings_value_str(char *out, size_t out_size, int idx) {
    switch (idx) {
        case 0: snprintf(out, out_size, "%s", k85_get_theme()->name); break;
        case 1: snprintf(out, out_size, "%d%%", g_config.brightness_active); break;
        case 2: snprintf(out, out_size, "%s", k85_battery_modes[g_config.battery_mode_idx]); break;
        case 3: {
            int bi = g_config.bootstyle_idx;
            if (bi < 0 || bi >= K85_BOOT_STYLE_COUNT) bi = 0;
            snprintf(out, out_size, "%s", k85_boot_style_names[bi]);
            break;
        }
        case 4: snprintf(out, out_size, "%s", k85_get_device_name()); break;
        case 5: snprintf(out, out_size, "%d%%", k85_get_sound_volume()); break;
        case 6:
            if (g_config.wifi_saved) {
                snprintf(out, out_size, "%s%s", g_config.wifi_ssid,
                         k85_wifi_is_connected() ? " (on)" : " (off)");
            } else {
                snprintf(out, out_size, "not set");
            }
            break;
        case 7: snprintf(out, out_size, "%d", g_config.step_count); break;
        case 8: out[0] = 0; break; // Back - без значения
        default: out[0] = 0;
    }
}

static void settings_draw(void) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(fg, bg);
    M5.Display.print("Settings");

    int y = 20;
    char val[40];
    for (int i = 0; i < K85_SETTINGS_ITEM_COUNT; i++) {
        bool sel = (i == s_selected);
        M5.Display.setCursor(6, y);
        M5.Display.setTextColor(sel ? accent : fg, bg);
        M5.Display.print(sel ? "> " : "  ");
        M5.Display.print(k85_settings_labels[i]);

        settings_value_str(val, sizeof(val), i);
        if (val[0]) {
            M5.Display.setCursor(140, y);
            M5.Display.print(val);
        }
        y += 14;
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(6, y + 6);
    M5.Display.print("A=next B=change/back A+B=exit");
}

static void settings_apply_item(int idx) {
    switch (idx) {
        case 0:
            g_config.theme_idx = (g_config.theme_idx + 1) % K85_THEME_COUNT;
            break;
        case 1:
            g_config.brightness_active += 10;
            if (g_config.brightness_active > 100) g_config.brightness_active = 10;
            M5.Display.setBrightness(g_config.brightness_active);
            break;
        case 2:
            g_config.battery_mode_idx = (g_config.battery_mode_idx + 1) % K85_BATTERY_MODE_COUNT;
            break;
        case 3:
            g_config.bootstyle_idx = (g_config.bootstyle_idx + 1) % K85_BOOT_STYLE_COUNT;
            break;
        case 4:
            g_config.device_name_idx = (g_config.device_name_idx + 1) % K85_DEVICE_NAME_COUNT;
            break;
        case 5: {
            int v = k85_get_sound_volume() + 10;
            if (v > 100) v = 0;
            k85_set_sound_volume(v);
            k85_apply_sound_volume();
            break;
        }
        case 6:
            if (g_config.wifi_saved) {
                k85_show_message("Connecting WiFi...");
                bool ok = k85_wifi_connect_saved();
                k85_show_message(ok ? "WiFi connected" : "WiFi failed");
                vTaskDelay(pdMS_TO_TICKS(800));
            } else {
                k85_show_message("No saved WiFi");
                vTaskDelay(pdMS_TO_TICKS(800));
            }
            break;
        case 7:
            k85_reset_step_counter();
            break;
        default:
            break;
    }
    k85_config_save();
}

void k85_run_settings_menu(void) {
    s_selected = 0;
    settings_draw();
    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            k85_config_save();
            return;
        }

        if (k85_btn_a_pressed()) {
            s_selected = (s_selected + 1) % K85_SETTINGS_ITEM_COUNT;
            settings_draw();
        }

        if (k85_btn_b_pressed()) {
            if (s_selected == K85_SETTINGS_BACK_IDX) {
                k85_config_save();
                return;
            }
            settings_apply_item(s_selected);
            settings_draw();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

