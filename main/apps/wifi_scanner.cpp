#include "wifi_scanner.h"
#include "common.h"
#include "input.h"
#include "theme.h"
#include "../net/wifi.h"

#include "M5Unified.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

#define K85_WIFI_SCAN_MAX 20

void k85_run_wifi_scanner(void) {
    k85_wifi_init();
    k85_show_message("Scanning WiFi...");

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true); // блокирующий

    if (err != ESP_OK) {
        k85_show_message("Scan failed\nA+B=back");
        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > K85_WIFI_SCAN_MAX) ap_count = K85_WIFI_SCAN_MAX;

    static wifi_ap_record_t records[K85_WIFI_SCAN_MAX];
    uint16_t actual = ap_count;
    esp_wifi_scan_get_ap_records(&actual, records);

    if (actual == 0) {
        k85_show_message("No networks found\nA+B=back");
        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        return;
    }

    int selected = 0;
    int scroll_top = 0;
    const int visible_rows = 8;
    uint32_t bg = k85_get_bg();

    while (true) {
        k85_input_update();

        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(k85_get_fg(), bg);
        M5.Display.setCursor(4, 4);
        M5.Display.printf("WiFi networks (%d)", actual);

        if (selected < scroll_top) scroll_top = selected;
        if (selected >= scroll_top + visible_rows) scroll_top = selected - visible_rows + 1;
        int last_visible = scroll_top + visible_rows;
        if (last_visible > actual) last_visible = actual;

        int y = 18;
        for (int i = scroll_top; i < last_visible; i++) {
            bool sel = (i == selected);
            M5.Display.setCursor(4, y);
            M5.Display.setTextColor(sel ? k85_get_accent() : k85_get_fg(), bg);
            M5.Display.print(sel ? "> " : "  ");
            M5.Display.printf("%-20.20s %ddBm", (const char *)records[i].ssid, records[i].rssi);
            y += 12;
        }

        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, y + 6);
        M5.Display.print("A=next A+B=exit");

        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed()) { selected = (selected + 1) % actual; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

