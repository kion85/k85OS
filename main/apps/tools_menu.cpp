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
#include "tools/task_manager.h"
#include "tools/lora_tool.h"
#include "tools/web_radio.h"
#include "tools/web_terminal.h"
#include "tools/level.h"
#include "tools/ping.h"
#include "tools/totp_auth.h"
#include "tools/password_manager.h"
#include "tools/orientation.h"
#include "tools/mqtt_tool.h"
#include "../net/kiwisdr_client.h"
#include "tools/disk_cleanup.h"
#include "tools/services.h"
#include "M5Unified.h"
#include <cstring>
#include <cmath>

static const char *TOOLS_ITEMS[] = {
    "WiFi Manager", "Color Test", "Bluetooth Scan", "I2C Scanner",
    "GPIO Control", "Files", "Music Player", "Melodies", "Mic Test",
    "Air Mouse (screen)", "Air Mouse BLE", "WiFi Hotspot", "Calculator", "Terminal", "IR Remote", "SSH Connect", "Task Manager", "LoRa", "Internet SDR", "Web Radio", "Web Terminal", "Level", "Ping", "Authenticator", "Password Manager", "Orientation", "MQTT", "Disk Cleanup", "Services", "Back"
};
#define TOOLS_COUNT (int)(sizeof(TOOLS_ITEMS) / sizeof(TOOLS_ITEMS[0]))

// Иконки под конкретные пункты Tools для grid/list+icons режимов
// (см. K85IconDrawFn в list_menu.h). По духу - как draw_menu_icon()
// в главном меню, только под набор инструментов.
static void draw_tools_icon(int cx, int cy, int r, const char *name, uint32_t col, uint32_t bg_col) {
    auto &d = M5.Display;
    if (!strcmp(name, "WiFi Manager")) {
        d.fillRect(cx - r/2, cy + r/2 - 2, 3, 3, col);
        d.fillRect(cx - r/6, cy + r/4 - 2, 3, r/2, col);
        d.fillRect(cx + r/6, cy - 2, 3, r - 2, col);
    } else if (!strcmp(name, "Color Test")) {
        d.fillCircle(cx - r/2, cy - r/3, r/3, 0xFF0000);
        d.fillCircle(cx + r/2, cy - r/3, r/3, 0x00FF00);
        d.fillCircle(cx, cy + r/3, r/3, 0x0000FF);
    } else if (!strcmp(name, "Bluetooth Scan")) {
        d.drawLine(cx, cy - r, cx, cy + r, col);
        d.drawLine(cx, cy - r, cx + r/2, cy - r/2, col);
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy + r/2, cx + r/2, cy + r/2, col);
        d.drawLine(cx + r/2, cy + r/2, cx - r/2, cy - r/2, col);
        d.drawLine(cx - r/2, cy - r/2, cx, cy - r, col);
    } else if (!strcmp(name, "I2C Scanner")) {
        d.drawCircle(cx - r/2, cy, r/3, col);
        d.drawCircle(cx + r/2, cy, r/3, col);
        d.drawLine(cx - r/2 + r/3, cy, cx + r/2 - r/3, cy, col);
    } else if (!strcmp(name, "GPIO Control")) {
        d.drawRect(cx - r/2, cy - r/2, r, r, col);
        for (int i = -1; i <= 1; i++) {
            d.drawLine(cx + i * r/3, cy - r/2, cx + i * r/3, cy - r, col);
            d.drawLine(cx + i * r/3, cy + r/2, cx + i * r/3, cy + r, col);
        }
    } else if (!strcmp(name, "Files")) {
        d.fillRect(cx - r, cy - r/3, r * 2, r, col);
        d.fillRect(cx - r, cy - r/2, r, r/4, col);
    } else if (!strcmp(name, "Music Player") || !strcmp(name, "Melodies")) {
        d.fillCircle(cx - r/3, cy + r/2, r/4, col);
        d.drawLine(cx - r/3 + r/4 - 1, cy + r/2, cx - r/3 + r/4 - 1, cy - r, col);
        d.drawLine(cx - r/3 + r/4 - 1, cy - r, cx + r/2, cy - r + r/4, col);
        d.drawLine(cx + r/2, cy - r + r/4, cx + r/2, cy + r/4, col);
        if (!strcmp(name, "Melodies")) {
            d.fillCircle(cx + r/2 - r/4, cy + r/4, r/4, col);
        }
    } else if (!strcmp(name, "Mic Test")) {
        d.fillRoundRect(cx - r/3, cy - r, r * 2 / 3, r, r/3, col);
        d.drawLine(cx, cy, cx, cy + r/2, col);
        d.drawLine(cx - r/3, cy + r/2, cx + r/3, cy + r/2, col);
        d.drawArc(cx, cy - r/2, r/2, r/2 + 2, 0, 180, col);
    } else if (!strcmp(name, "Air Mouse (screen)") || !strcmp(name, "Air Mouse BLE")) {
        d.fillTriangle(cx - r/2, cy - r, cx - r/2, cy + r/2, cx, cy + r/6, col);
        d.fillTriangle(cx - r/2, cy + r/2, cx, cy + r/6, cx - r/6, cy + r, col);
        if (!strcmp(name, "Air Mouse BLE")) {
            d.drawLine(cx + r/4, cy - r, cx + r/4, cy, col);
            d.drawLine(cx + r/4, cy - r, cx + r/2, cy - r/2, col);
            d.drawLine(cx + r/2, cy - r/2, cx, cy - r/4, col);
        }
    } else if (!strcmp(name, "WiFi Hotspot")) {
        d.fillRect(cx - r/2, cy + r/2 - 2, 3, 3, col);
        d.fillRect(cx - r/6, cy + r/4 - 2, 3, r/2, col);
        d.fillRect(cx + r/6, cy - 2, 3, r - 2, col);
        d.drawCircle(cx, cy, r, col);
    } else if (!strcmp(name, "Calculator")) {
        d.drawRect(cx - r/2, cy - r, r, r * 2, col);
        for (int row = 0; row < 3; row++) {
            for (int c = 0; c < 2; c++) {
                d.fillRect(cx - r/2 + 3 + c * (r/2 - 2), cy - r/2 + row * (r/2), 4, 4, col);
            }
        }
    } else if (!strcmp(name, "Terminal") || !strcmp(name, "Web Terminal")) {
        d.drawRect(cx - r, cy - r/2, r * 2, r, col);
        d.drawLine(cx - r + 4, cy - r/4, cx - r/2, cy, col);
        d.drawLine(cx - r/2, cy, cx - r + 4, cy + r/4, col);
        d.drawLine(cx - r/3, cy + r/4, cx, cy + r/4, col);
    } else if (!strcmp(name, "IR Remote")) {
        d.drawRoundRect(cx - r/3, cy - r, r * 2 / 3, r * 2, r/4, col);
        d.fillCircle(cx, cy - r/2, 2, col);
        d.drawArc(cx, cy - r, r/2, r/2 + 2, 200, 340, col);
    } else if (!strcmp(name, "SSH Connect")) {
        d.drawRoundRect(cx - r, cy - r/2, r * 2, r, r/4, col);
        d.drawLine(cx - r/2, cy - r/6, cx - r/4, cy, col);
        d.drawLine(cx - r/4, cy, cx - r/2, cy + r/6, col);
        d.fillRect(cx, cy + r/6 - 1, r/3, 2, col);
    } else if (!strcmp(name, "Task Manager")) {
        for (int i = 0; i < 3; i++) {
            d.drawRect(cx - r, cy - r + i * (2 * r / 3) + 2, r * 2, r/2, col);
        }
    } else if (!strcmp(name, "LoRa")) {
        d.fillCircle(cx, cy, 2, col);
        d.drawArc(cx, cy, r/2, r/2 + 2, 0, 360, col);
        d.drawArc(cx, cy, r, r + 2, 0, 360, col);
    } else if (!strcmp(name, "Internet SDR")) {
        d.drawCircle(cx, cy, r, col);
        d.drawLine(cx, cy, cx + r/2, cy - r/2, col);
        for (int a = 0; a < 360; a += 90) {
            float rad = a * 3.14159f / 180.0f;
            int x1 = cx + (int)(cosf(rad) * (r - 2));
            int y1 = cy + (int)(sinf(rad) * (r - 2));
            d.drawPixel(x1, y1, col);
        }
    } else if (!strcmp(name, "Web Radio")) {
        d.fillRect(cx - r/2, cy, r, r/2, col);
        d.drawLine(cx - r/4, cy, cx, cy - r/2, col);
        d.drawLine(cx, cy - r/2, cx + r/3, cy, col);
        d.fillCircle(cx - r/4, cy - r/2, 2, col);
    } else if (!strcmp(name, "Level")) {
        d.drawCircle(cx, cy, r, col);
        d.fillCircle(cx + r/4, cy - r/5, r/4, col);
    } else if (!strcmp(name, "Authenticator")) {
        d.drawRoundRect(cx - r/2, cy - r/2, r, r, r/4, col);
        d.fillCircle(cx, cy - r/6, r/4, col);
        d.fillRect(cx - 1, cy, 2, r/3, col);
    } else if (!strcmp(name, "Password Manager")) {
        d.drawRoundRect(cx - r/2, cy - r/6, r, r * 2 / 3, r/6, col);
        d.drawArc(cx, cy - r/3, r/3, r/3 + 2, 180, 360, col);
        d.fillCircle(cx, cy + r/6, 2, col);
    } else if (!strcmp(name, "Orientation")) {
        d.drawRect(cx - r/2, cy - r/2, r, r, col);
        d.drawRect(cx - r/2 + r/4, cy - r/2 - r/4, r, r, col);
        d.drawLine(cx - r/2, cy - r/2, cx - r/2 + r/4, cy - r/2 - r/4, col);
        d.drawLine(cx + r/2, cy - r/2, cx + r/2 + r/4, cy - r/2 - r/4, col);
    } else if (!strcmp(name, "Ping")) {
        // Радар-волны, отражающиеся от вертикальной "стены" (хоста).
        d.drawFastVLine(cx + r/2, cy - r, r * 2, col);
        d.drawArc(cx - r/4, cy, r/3, r/3 + 2, -60, 60, col);
        d.drawArc(cx - r/4, cy, r * 2 / 3, r * 2 / 3 + 2, -60, 60, col);
    } else if (!strcmp(name, "MQTT")) {
        d.fillCircle(cx, cy, r/3, col);
        d.drawArc(cx, cy, r/2, r/2 + 2, 200, 340, col);
        d.drawArc(cx, cy, r, r + 2, 200, 340, col);
    } else if (!strcmp(name, "Disk Cleanup")) {
        d.drawCircle(cx, cy, r, col);
        d.fillRect(cx - r/2, cy - 1, r, 2, col);
        d.fillTriangle(cx - r/3, cy, cx + r/3, cy, cx, cy + r/2, col);
    } else if (!strcmp(name, "Services")) {
        d.fillCircle(cx, cy, r/3, col);
        d.drawArc(cx, cy, r/2, r/2 + 2, 200, 340, col);
        d.drawArc(cx, cy, r, r + 2, 200, 340, col);
    } else if (!strcmp(name, "Back")) {
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy, col);
        d.drawLine(cx - r/2, cy, cx + r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy, cx + r, cy, col);
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
}

void k85_run_tools_menu(void) {
    while (true) {
        int idx = k85_run_list_menu("TOOLS", TOOLS_ITEMS, TOOLS_COUNT, nullptr, draw_tools_icon);
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
            else if (idx == 16) k85_run_task_manager();
        else if (idx == 17) k85_run_lora_tool();
        else if (idx == 18) k85_run_kiwisdr_client();
        else if (idx == 19) k85_run_web_radio();
        else if (idx == 20) k85_run_web_terminal();
        else if (idx == 21) k85_run_level();
        else if (idx == 22) {
            if (g_config.wifi_disabled) {
                k85_show_message("WiFi disabled\n(k85os-menu)");
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                k85_run_ping();
            }
        }
        else if (idx == 23) k85_run_totp_auth();
        else if (idx == 24) k85_run_password_manager();
        else if (idx == 25) k85_run_orientation();
        else if (idx == 26) k85_run_mqtt_tool();
        else if (idx == 27) k85_run_disk_cleanup();
        else if (idx == 28) k85_run_services();
    }
}
