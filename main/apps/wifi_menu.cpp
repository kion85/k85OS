#include "wifi_menu.h"
#include "common.h"
#include "theme.h"
#include "input.h"
#include "config.h"
#include "list_menu.h"
#include "../net/wifi.h"
#include "tools/wifi_hotspot.h"
#include "M5Unified.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void show_network_info(void) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    bool connected = k85_wifi_is_connected();
    char msg[220];
    if (connected) {
        snprintf(msg, sizeof(msg),
            "SSID: %s\nIP: %s\nRSSI: %d dBm\nMAC: %02X:%02X:%02X:%02X:%02X:%02X\nA+B=back",
            g_config.wifi_ssid, k85_wifi_get_ip_str(), k85_wifi_get_rssi(),
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(msg, sizeof(msg),
            "Status: Disconnected\nSaved SSID: %s\nMAC: %02X:%02X:%02X:%02X:%02X:%02X\nA+B=back",
            g_config.wifi_saved ? g_config.wifi_ssid : "(none)",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    k85_show_message(msg);
    wait_ab_exit();
}

// Иконки для подменю Hotspot Settings. Обратите внимание: подписи тут
// динамические ("Channel: 6", "Security: WPA2"), поэтому сравниваем через
// strncmp по префиксу вместо strcmp по всей строке.
static void draw_hotspot_icon(int cx, int cy, int r, const char *name, uint32_t col, uint32_t bg_col) {
    auto &d = M5.Display;
    if (!strncmp(name, "Start Hotspot", 13)) {
        d.fillRect(cx - r/2, cy + r/2 - 2, 3, 3, col);
        d.fillRect(cx - r/6, cy + r/4 - 2, 3, r/2, col);
        d.fillRect(cx + r/6, cy - 2, 3, r - 2, col);
        d.drawCircle(cx, cy, r, col);
    } else if (!strncmp(name, "Channel", 7)) {
        // Три вертикальные "радиоволны" разной высоты.
        d.fillRect(cx - r/2, cy + r/3, 3, r/3, col);
        d.fillRect(cx - r/6, cy, 3, r * 2 / 3, col);
        d.fillRect(cx + r/6, cy - r/3, 3, r, col);
    } else if (!strncmp(name, "Security", 8)) {
        // Замок.
        d.drawRoundRect(cx - r/2, cy - r/6, r, r * 2 / 3, r/6, col);
        d.drawArc(cx, cy - r/3, r/3, r/3 + 2, 180, 360, col);
    } else if (!strcmp(name, "Back")) {
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy, col);
        d.drawLine(cx - r/2, cy, cx + r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy, cx + r, cy, col);
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
}

static void run_hotspot_settings(void) {
    static const char *items[] = {"Start Hotspot", "Channel", "Security", "Back"};
    while (true) {
        char ch_str[8], sec_str[8];
        snprintf(ch_str, sizeof(ch_str), "%d", g_config.ap_channel);
        snprintf(sec_str, sizeof(sec_str), "%s", g_config.ap_open ? "Open" : "WPA2");
        char labeled[4][40];
        snprintf(labeled[0], sizeof(labeled[0]), "%s", items[0]);
        snprintf(labeled[1], sizeof(labeled[1]), "%s: %s", items[1], ch_str);
        snprintf(labeled[2], sizeof(labeled[2]), "%s: %s", items[2], sec_str);
        snprintf(labeled[3], sizeof(labeled[3]), "%s", items[3]);
        const char *display[4] = { labeled[0], labeled[1], labeled[2], labeled[3] };
        int idx = k85_run_list_menu("HOTSPOT SETTINGS", display, 4, nullptr, draw_hotspot_icon);
        if (idx < 0 || idx == 3) return;
        if (idx == 0) {
            k85_run_wifi_hotspot();
        } else if (idx == 1) {
            g_config.ap_channel = (g_config.ap_channel % 13) + 1;
            k85_config_save();
        } else if (idx == 2) {
            g_config.ap_open = !g_config.ap_open;
            k85_config_save();
        }
    }
}

// Иконки главного WiFi-меню.
static void draw_wifi_menu_icon(int cx, int cy, int r, const char *name, uint32_t col, uint32_t bg_col) {
    auto &d = M5.Display;
    if (!strcmp(name, "Network info")) {
        d.drawCircle(cx, cy, r, col);
        d.fillRect(cx - 1, cy - r/3, 2, r/3, col);
        d.fillRect(cx - 1, cy - r/2 - 2, 2, 2, col);
    } else if (!strcmp(name, "Hotspot")) {
        d.fillRect(cx - r/2, cy + r/2 - 2, 3, 3, col);
        d.fillRect(cx - r/6, cy + r/4 - 2, 3, r/2, col);
        d.fillRect(cx + r/6, cy - 2, 3, r - 2, col);
        d.drawCircle(cx, cy, r, col);
    } else if (!strcmp(name, "Back")) {
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy, col);
        d.drawLine(cx - r/2, cy, cx + r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy, cx + r, cy, col);
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
}

void k85_run_wifi_menu(void) {
    static const char *items[] = {"Network info", "Hotspot", "Back"};
    while (true) {
        int idx = k85_run_list_menu("WIFI", items, 3, nullptr, draw_wifi_menu_icon);
        if (idx < 0 || idx == 2) return;
        if (idx == 0) show_network_info();
        else if (idx == 1) run_hotspot_settings();
    }
}