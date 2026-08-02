#include "bt_scan.h"
#include "common.h"
#include "text_input.h"
#include "log.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

struct BtFound { uint8_t addr[6]; int rssi; };
static BtFound s_found[16];
static int s_found_n = 0;
static EventGroupHandle_t s_bt_event_group = nullptr;
static const int BT_SCAN_DONE_BIT = BIT0;

static bool addr_seen(const uint8_t *addr) {
    for (int i = 0; i < s_found_n; i++)
        if (!memcmp(s_found[i].addr, addr, 6)) return true;
    return false;
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    if (event == ESP_GAP_BLE_SCAN_RESULT_EVT) {
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            if (s_found_n < 16 && !addr_seen(param->scan_rst.bda)) {
                memcpy(s_found[s_found_n].addr, param->scan_rst.bda, 6);
                s_found[s_found_n].rssi = param->scan_rst.rssi;
                s_found_n++;
            }
        } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            if (s_bt_event_group) xEventGroupSetBits(s_bt_event_group, BT_SCAN_DONE_BIT);
        }
    }
}

void k85_run_bt_scan(void) {
    static bool bt_inited = false;
    s_found_n = 0;

    if (!bt_inited) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        if (esp_bt_controller_init(&bt_cfg) != ESP_OK ||
            esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK ||
            esp_bluedroid_init() != ESP_OK ||
            esp_bluedroid_enable() != ESP_OK) {
            k85_log("BT init failed - check sdkconfig (Bluetooth/Bluedroid enabled?)");
            k85_show_message("BT scan not\nsupported here");
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
        esp_ble_gap_register_callback(gap_cb);
        bt_inited = true;
    }

    if (!s_bt_event_group) s_bt_event_group = xEventGroupCreate();
    xEventGroupClearBits(s_bt_event_group, BT_SCAN_DONE_BIT);

    esp_ble_scan_params_t scan_params = {};
    scan_params.scan_type = BLE_SCAN_TYPE_PASSIVE;
    scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    scan_params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    scan_params.scan_interval = 0x50;
    scan_params.scan_window = 0x30;
    esp_ble_gap_set_scan_params(&scan_params);

    k85_show_message("Scanning BLE...");
    esp_ble_gap_start_scanning(4); // 4 секунды, как в оригинале

    xEventGroupWaitBits(s_bt_event_group, BT_SCAN_DONE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));

    char lines_buf[9][32];
    const char *lines[9];
    int n = 0;
    snprintf(lines_buf[n], 32, "Found: %d", s_found_n);
    lines[n] = lines_buf[n]; n++;
    for (int i = 0; i < s_found_n && n < 9; i++) {
        snprintf(lines_buf[n], 32, "%02x:%02x:%02x:%02x:%02x:%02x %d",
                 s_found[i].addr[0], s_found[i].addr[1], s_found[i].addr[2],
                 s_found[i].addr[3], s_found[i].addr[4], s_found[i].addr[5],
                 s_found[i].rssi);
        lines[n] = lines_buf[n]; n++;
    }
    k85_area_show(lines, n, "BLUETOOTH");
}

