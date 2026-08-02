#include "wifi_manager.h"
#include "wifi.h"
#include "config.h"
#include "common.h"
#include "text_input.h"
#include "list_menu.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static void show_status(void) {
    if (k85_wifi_is_connected()) {
        char l0[96], l1[96], l2[40];
        snprintf(l0, sizeof(l0), "IP: %s", k85_wifi_get_ip_str());
        snprintf(l1, sizeof(l1), "SSID: %s", g_config.wifi_ssid);
        snprintf(l2, sizeof(l2), "RSSI: %d dBm", k85_wifi_get_rssi());
        const char *lines[3] = {l0, l1, l2};
        k85_area_show(lines, 3, "WiFi Status");
    } else {
        k85_show_message("Not connected\nA+B=back");
    }
}

static void scan_and_connect(void) {
    k85_show_message("Scanning...");
    char ssids[16][33];
    int n = k85_wifi_scan(ssids, 16);
    if (n == 0) {
        k85_show_message("No networks found\nA+B=back");
        return;
    }

    const char *items[17];
    for (int i = 0; i < n; i++) items[i] = ssids[i];
    items[n] = "Back";

    int idx = k85_run_list_menu("WIFI NETWORKS", items, n + 1, nullptr);
    if (idx < 0 || idx >= n) return;

    char password[64] = "";
    if (!k85_text_input("Enter password:", "", password, sizeof(password))) return;

    k85_show_message("Connecting...");
    bool ok = k85_wifi_connect(ssids[idx], password);
    if (ok) {
        snprintf(g_config.wifi_ssid, sizeof(g_config.wifi_ssid), "%s", ssids[idx]);
        snprintf(g_config.wifi_password, sizeof(g_config.wifi_password), "%s", password);
        g_config.wifi_saved = true;
        k85_config_save();
        char buf[48];
        snprintf(buf, sizeof(buf), "Connected!\n%s", k85_wifi_get_ip_str());
        k85_show_message(buf);
    } else {
        k85_show_message("Connect failed\n(timeout)");
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
}

static void forget_current(void) {
    k85_show_message("Forgetting...");
    k85_wifi_disconnect();
    g_config.wifi_ssid[0] = 0;
    g_config.wifi_password[0] = 0;
    g_config.wifi_saved = false;
    k85_config_save();
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void k85_run_wifi_manager(void) {
    const char *items[] = {"Status", "Scan & connect", "Forget current", "Back"};
    while (true) {
        int idx = k85_run_list_menu("WiFi Manager", items, 4, nullptr);
        if (idx < 0 || idx == 3) return;
        if (idx == 0) show_status();
        else if (idx == 1) scan_and_connect();
        else if (idx == 2) forget_current();
    }
}


