#include "k85os_menu.h"
#include "common.h"
#include "theme.h"
#include "battery.h"
#include "input.h"
#include "config.h"
#include "wifi.h"

#include "M5Unified.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_littlefs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

#define K85_BIOS_ITEM_COUNT 10
#define K85_BIOS_VISIBLE_ROWS 6

static const char *k85_bios_labels[K85_BIOS_ITEM_COUNT] = {
    "WiFi module", "Bluetooth module", "OTA lock (secure)",
    "RAM / ROM info", "Chip info", "Uptime",
    "Wipe WiFi networks", "Reset config", "Factory reset",
    "Reboot",
};

static int s_selected = 0;
static int s_scroll_top = 0;

static void bios_clamp_scroll(void) {
    if (s_selected < s_scroll_top) s_scroll_top = s_selected;
    if (s_selected >= s_scroll_top + K85_BIOS_VISIBLE_ROWS) s_scroll_top = s_selected - K85_BIOS_VISIBLE_ROWS + 1;
    if (s_scroll_top < 0) s_scroll_top = 0;
    int max_top = K85_BIOS_ITEM_COUNT - K85_BIOS_VISIBLE_ROWS;
    if (max_top < 0) max_top = 0;
    if (s_scroll_top > max_top) s_scroll_top = max_top;
}

static void bios_value_str(char *out, size_t out_size, int idx) {
    switch (idx) {
        case 0: snprintf(out, out_size, "%s", g_config.wifi_disabled ? "OFF" : "ON"); break;
        case 1: snprintf(out, out_size, "%s", g_config.bt_disabled ? "OFF" : "ON"); break;
        case 2: snprintf(out, out_size, "%s", g_config.ota_locked ? "LOCKED" : "unlocked"); break;
        default: out[0] = 0;
    }
}

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void show_ram_rom_info(void) {
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);
    unsigned used_kb = (unsigned)(heap_info.total_allocated_bytes / 1024);
    unsigned total_kb = (unsigned)((heap_info.total_allocated_bytes + heap_info.total_free_bytes) / 1024);

    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);

    size_t fs_total = 0, fs_used = 0;
    bool fs_ok = k85_fs_info(&fs_total, &fs_used);

    char msg[160];
    if (fs_ok) {
        snprintf(msg, sizeof(msg),
            "RAM: %u/%u KB\nFlash: %u MB\nLittleFS: %u/%u KB\nA+B=back",
            used_kb, total_kb, (unsigned)(flash_size / (1024 * 1024)),
            (unsigned)(fs_used / 1024), (unsigned)(fs_total / 1024));
    } else {
        snprintf(msg, sizeof(msg),
            "RAM: %u/%u KB\nFlash: %u MB\nA+B=back",
            used_kb, total_kb, (unsigned)(flash_size / (1024 * 1024)));
    }
    k85_show_message(msg);
    wait_ab_exit();
}

static void show_chip_info(void) {
    esp_chip_info_t info;
    esp_chip_info(&info);
    char msg[128];
    snprintf(msg, sizeof(msg),
        "Model: ESP32-S3\nRev: v%d.%d\nCores: %d\nFreq: %d MHz\nA+B=back",
        info.revision / 100, info.revision % 100, info.cores, 240);
    k85_show_message(msg);
    wait_ab_exit();
}

static void show_uptime(void) {
    int64_t us = esp_timer_get_time();
    int64_t s = us / 1000000;
    int h = (int)(s / 3600);
    int m = (int)((s % 3600) / 60);
    int sec = (int)(s % 60);
    char msg[64];
    snprintf(msg, sizeof(msg), "Uptime: %02d:%02d:%02d\nA+B=back", h, m, sec);
    k85_show_message(msg);
    wait_ab_exit();
}

static bool confirm_action(const char *label) {
    char msg[80];
    snprintf(msg, sizeof(msg), "%s?\nB=confirm A+B=cancel", label);
    k85_show_message(msg);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) return true;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void bios_draw(void) {
    bios_clamp_scroll();
    uint32_t bg = 0x000000;
    uint32_t fg = 0xFFFFFF;
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(accent, bg);
    M5.Display.print("k85os-menu");

    int y = 20;
    char val[32];
    int last_visible = s_scroll_top + K85_BIOS_VISIBLE_ROWS;
    if (last_visible > K85_BIOS_ITEM_COUNT) last_visible = K85_BIOS_ITEM_COUNT;

    for (int i = s_scroll_top; i < last_visible; i++) {
        bool sel = (i == s_selected);
        M5.Display.setCursor(6, y);
        M5.Display.setTextColor(sel ? accent : fg, bg);
        M5.Display.print(sel ? "> " : "  ");
        M5.Display.print(k85_bios_labels[i]);

        bios_value_str(val, sizeof(val), i);
        if (val[0]) {
            M5.Display.setCursor(150, y);
            M5.Display.print(val);
        }
        y += 14;
    }

    M5.Display.setTextColor(0x777777, bg);
    M5.Display.setCursor(6, y + 6);
    M5.Display.print("A=next B=select A+B=exit");

    k85_draw_battery_icon();
}

static void bios_apply(int idx) {
    switch (idx) {
        case 0:
            g_config.wifi_disabled = !g_config.wifi_disabled;
            if (g_config.wifi_disabled) k85_wifi_disconnect();
            k85_config_save();
            break;
        case 1:
            g_config.bt_disabled = !g_config.bt_disabled;
            k85_config_save();
            break;
        case 2:
            g_config.ota_locked = !g_config.ota_locked;
            k85_config_save();
            break;
        case 3: show_ram_rom_info(); break;
        case 4: show_chip_info(); break;
        case 5: show_uptime(); break;
        case 6:
            if (confirm_action("Wipe WiFi networks")) {
                g_config.wifi_saved = false;
                g_config.wifi_ssid[0] = 0;
                g_config.wifi_password[0] = 0;
                g_config.wifi_networks_count = 0;
                k85_config_save();
                k85_show_message("WiFi networks wiped\nA+B=back");
                wait_ab_exit();
            }
            break;
        case 7:
            if (confirm_action("Reset config to defaults")) {
                k85_config_defaults(&g_config);
                k85_config_save();
                k85_show_message("Config reset\nA+B=back");
                wait_ab_exit();
            }
            break;
        case 8:
            if (confirm_action("FACTORY RESET (wipe all data)")) {
                k85_config_defaults(&g_config);
                k85_config_save();
                esp_littlefs_format(K85_LITTLEFS_PART_LABEL);
                k85_show_message("Factory reset done\nRebooting...");
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
            break;
        case 9:
            if (confirm_action("Reboot device")) {
                esp_restart();
            }
            break;
        default:
            break;
    }
}

void k85_run_bios_menu(void) {
    s_selected = 0;
    s_scroll_top = 0;
    bios_draw();

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        if (k85_btn_a_pressed()) {
            s_selected = (s_selected + 1) % K85_BIOS_ITEM_COUNT;
            bios_draw();
        }
        if (k85_btn_b_pressed()) {
            bios_apply(s_selected);
            bios_draw();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
