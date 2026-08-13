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
#include "../net/ota.h"
#include "../core/version.h"
#include "text_input.h"
#include "status_bar_settings.h"
#include "lock_screen_settings.h"
#include "../core/lock_auth.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstddef>

#define K85_SETTINGS_ITEM_COUNT 14
#define K85_SETTINGS_BACK_IDX   (K85_SETTINGS_ITEM_COUNT - 1)

static const char *k85_settings_labels[K85_SETTINGS_ITEM_COUNT] = {
    "Theme", "Brightness", "Battery mode", "Boot style",
    "Device name", "Sound volume", "WiFi", "Reset steps",
    "Check for updates", "Screen lock", "Status bar", "BG gradient", "Lock screen", "Back",
};

static int s_selected = 0;
static int s_scroll_top = 0;
#define K85_SETTINGS_VISIBLE_ROWS 6

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
            if (g_config.wifi_disabled) {
                k85_show_message("WiFi module disabled\n(k85os-menu)");
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            }
            if (g_config.wifi_saved) {
                snprintf(out, out_size, "%s%s", g_config.wifi_ssid,
                         k85_wifi_is_connected() ? " (on)" : " (off)");
            } else {
                snprintf(out, out_size, "not set");
            }
            break;
        case 7: snprintf(out, out_size, "%d", g_config.step_count); break;
        case 8: snprintf(out, out_size, "v%s", K85_FW_VERSION); break;
        case 9: snprintf(out, out_size, "%s", g_config.lock_enabled ? "ON" : "OFF"); break;
        case 10: out[0] = 0; break;
        case 11: snprintf(out, out_size, "%s", g_config.bg_gradient_enabled ? "ON" : "OFF"); break;
        case 12: out[0] = 0; break;
        case 13: out[0] = 0; break; // Back - без значения
        default: out[0] = 0;
    }
}

static void settings_clamp_scroll(void) {
    if (s_selected < s_scroll_top) {
        s_scroll_top = s_selected;
    }
    if (s_selected >= s_scroll_top + K85_SETTINGS_VISIBLE_ROWS) {
        s_scroll_top = s_selected - K85_SETTINGS_VISIBLE_ROWS + 1;
    }
    if (s_scroll_top < 0) s_scroll_top = 0;
    int max_top = K85_SETTINGS_ITEM_COUNT - K85_SETTINGS_VISIBLE_ROWS;
    if (max_top < 0) max_top = 0;
    if (s_scroll_top > max_top) s_scroll_top = max_top;
}

static void settings_draw(void) {
    settings_clamp_scroll();

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
    int last_visible = s_scroll_top + K85_SETTINGS_VISIBLE_ROWS;
    if (last_visible > K85_SETTINGS_ITEM_COUNT) last_visible = K85_SETTINGS_ITEM_COUNT;

    for (int i = s_scroll_top; i < last_visible; i++) {
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

    // Индикатор скролла справа
    if (K85_SETTINGS_ITEM_COUNT > K85_SETTINGS_VISIBLE_ROWS) {
        if (s_scroll_top > 0) {
            M5.Display.setCursor(230, 20);
            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.print("^");
        }
        if (last_visible < K85_SETTINGS_ITEM_COUNT) {
            M5.Display.setCursor(230, 20 + (K85_SETTINGS_VISIBLE_ROWS - 1) * 14);
            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.print("v");
        }
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(6, y + 6);
    M5.Display.print("A=next B=change/back A+B=exit");
}

static void settings_apply_item(int idx) {
    switch (idx) {
        case 0:
            g_config.theme_idx = (g_config.theme_idx + 1) % k85_theme_count();
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
            if (g_config.wifi_disabled) {
                k85_show_message("WiFi module disabled\n(k85os-menu)");
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            }
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
        case 8: {
            if (!k85_wifi_is_connected()) {
                k85_show_message("Connect WiFi first\nA+B=back");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                break;
            }
            char ver[16];
            char url[256];
            k85_show_message("Checking...");
            if (!k85_ota_check_update(ver, sizeof(ver), url, sizeof(url))) {
                k85_show_message("No update found\nA+B=back");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                break;
            }

            char confirm_msg[64];
            snprintf(confirm_msg, sizeof(confirm_msg), "Update v%s found\nB=install A+B=cancel", ver);
            k85_show_message(confirm_msg);

            bool do_install = false;
            while (true) {
                k85_input_update();
                if (k85_ab_held(500)) { k85_wait_ab_release(); do_install = false; break; }
                if (k85_btn_b_pressed()) { do_install = true; break; }
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            if (!do_install) break;

            k85_show_message("Installing 0%...");
            static char progress_msg[32];
            bool ok = k85_ota_perform_update(url, [](int percent) {
                snprintf(progress_msg, sizeof(progress_msg), "Installing %d%%...", percent);
                k85_show_message(progress_msg);
            });
            // Если дошли сюда — не удалось (успех перезагружает устройство сам)
            if (!ok) {
                k85_show_message("Update failed\nA+B=back");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }
            break;
        }
        case 9: {
            if (g_config.lock_enabled) {
                k85_show_message("Disable screen lock?\nB=confirm A+B=cancel");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); goto lock_done; }
                    if (k85_btn_b_pressed()) break;
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                g_config.lock_enabled = false;
                g_config.lock_password[0] = 0;
                k85_show_message("Screen lock disabled\nA+B=back");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            } else {
                char pass[32] = "";
                if (k85_text_input("Set lock password:", "", pass, sizeof(pass)) && pass[0]) {
                    k85_lock_hash_password(pass, g_config.lock_password, sizeof(g_config.lock_password));
                    g_config.lock_enabled = true;
                    k85_show_message("Screen lock enabled\nA+B=back");
                    while (true) {
                        k85_input_update();
                        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                        vTaskDelay(pdMS_TO_TICKS(30));
                    }
                }
            }
            lock_done:
            break;
        }
        case 10:
            k85_run_status_bar_settings();
            break;
        case 11:
            g_config.bg_gradient_enabled = !g_config.bg_gradient_enabled;
            break;
        case 12:
            k85_run_lock_screen_settings();
            break;
        default:
            break;
    }
    k85_config_save();
}

void k85_run_settings_menu(void) {
    s_selected = 0;
    s_scroll_top = 0;
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
















