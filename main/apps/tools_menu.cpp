#include "tools_menu.h"
#include "list_menu.h"
#include "common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tools/wifi_manager.h"
#include "tools/i2c_scanner.h"
#include "tools/gpio_control.h"
#include "tools/color_test.h"
#include "tools/calculator.h"
#include "tools/melodies.h"
#include "tools/air_mouse.h"
#include "../ble_hid/air_mouse_ble.h"
#include "core/config.h"
#include "tools/files.h"
#include "tools/music_player.h"
#include "tools/mic_test.h"
#include "tools/wifi_hotspot.h"
#include "tools/bt_scan.h"
#include "tools/terminal.h"
#include "tools/ir_remote.h"
#include "../net/ssh_client.h"

static const char *TOOLS_ITEMS[] = {
    "WiFi Manager", "Color Test", "Bluetooth Scan", "I2C Scanner",
    "GPIO Control", "Files", "Music Player", "Melodies", "Mic Test",
    "Air Mouse (screen)", "Air Mouse BLE", "WiFi Hotspot", "Calculator", "Terminal", "IR Remote", "SSH Connect", "Back"
};
#define TOOLS_COUNT (int)(sizeof(TOOLS_ITEMS) / sizeof(TOOLS_ITEMS[0]))

void k85_run_tools_menu(void) {
    while (true) {
        int idx = k85_run_list_menu("TOOLS", TOOLS_ITEMS, TOOLS_COUNT, nullptr);
        if (idx < 0 || idx == TOOLS_COUNT - 1) return;
        if (idx == 0) {
            if (g_config.wifi_disabled) {
                k85_show_message("WiFi disabled\n(k85os-menu)");
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                k85_run_wifi_manager();
            }
        }
        else if (idx == 1) k85_run_color_test();
        else if (idx == 2) k85_run_bt_scan();
        else if (idx == 3) k85_run_i2c_scan();
        else if (idx == 4) k85_run_gpio_test();
        else if (idx == 5) k85_run_files();
        else if (idx == 6) k85_run_music_player();
        else if (idx == 7) k85_run_melody_player();
        else if (idx == 8) k85_run_mic_test();
        else if (idx == 9) k85_run_air_mouse();
        else if (idx == 10) {
            if (g_config.bt_disabled) {
                k85_show_message("Bluetooth disabled\n(k85os-menu)");
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                k85_run_air_mouse_ble();
            }
        }
        else if (idx == 11) {
            if (g_config.wifi_disabled) {
                k85_show_message("WiFi disabled\n(k85os-menu)");
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                k85_run_wifi_hotspot();
            }
        }
        else if (idx == 12) k85_run_calculator();
        else if (idx == 13) { if (k85_run_terminal()) return; }
        else if (idx == 14) k85_run_ir_remote();
        else if (idx == 15) {
            if (g_config.wifi_disabled) {
                k85_show_message("WiFi disabled\n(k85os-menu)");
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                k85_run_ssh_client();
            }
        }
    }
}








