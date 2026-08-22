#include "air_mouse_ble.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "common.h"
#include "../net/wifi.h"
#include "../core/config.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_hidd_prf_api.h"
#include "hid_dev.h"
#include "nvs_flash.h"

static bool s_hid_inited = false;
static bool s_connected = false;
static uint16_t s_conn_id = 0;

#define K85_MOUSE_APPEARANCE 0x03C2  // "Bluetooth Mouse" in Windows/Android UI

static uint8_t k85_hidd_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t k85_hidd_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = K85_MOUSE_APPEARANCE,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(k85_hidd_service_uuid128),
    .p_service_uuid = k85_hidd_service_uuid128,
    .flag = 0x6,
};

static esp_ble_adv_params_t k85_hidd_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void k85_hidd_event_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param) {
    switch (event) {
        case ESP_HIDD_EVENT_REG_FINISH:
            if (param->init_finish.state == ESP_HIDD_INIT_OK) {
                esp_ble_gap_set_device_name("k85 Air Mouse");
                esp_ble_gap_config_adv_data(&k85_hidd_adv_data);
            }
            break;
        case ESP_HIDD_EVENT_BLE_CONNECT:
            s_conn_id = param->connect.conn_id;
            s_connected = true;
            break;
        case ESP_HIDD_EVENT_BLE_DISCONNECT:
            s_connected = false;
            esp_ble_gap_start_advertising(&k85_hidd_adv_params);
            break;
        default:
            break;
    }
}

static void k85_hidd_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            esp_ble_gap_start_advertising(&k85_hidd_adv_params);
            break;
        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;
        default:
            break;
    }
}

static bool k85_hid_stack_init(void) {
    if (s_hid_inited) return true;

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        // Освобождаем память под Classic BT — используется только BLE,
        // а держать резерв под классику (десятки КБ internal RAM) смысла нет.
        // Именно нехватка internal RAM в этот момент роняла esp_bt_controller_enable
        // с "Malloc failed" -> Guru Meditation.
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        if (esp_bt_controller_init(&bt_cfg) != ESP_OK) return false;
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) return false;
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        if (esp_bluedroid_init() != ESP_OK) return false;
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        if (esp_bluedroid_enable() != ESP_OK) return false;
    }
    if (esp_hidd_profile_init() != ESP_OK) return false;

    esp_ble_gap_register_callback(k85_hidd_gap_cb);
    esp_hidd_register_callbacks(k85_hidd_event_cb);

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    s_hid_inited = true;
    return true;
}

void k85_run_air_mouse_ble(void) {
    // Освобождаем internal RAM от WiFi-драйвера перед BLE-инициализацией —
    // без этого BLE-контроллер может уронить CORRUPT HEAP из-за
    // фрагментации памяти (WiFi+BLE вместе тесно на этой памяти).
    k85_wifi_stop();

    if (!k85_hid_stack_init()) {
        k85_show_message("BLE HID init\nfailed");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    float ax0, ay0, az0;
    if (!M5.Imu.getAccel(&ax0, &ay0, &az0)) {
        k85_show_message("IMU not\navailable");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    auto draw_status = [&]() {
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.print("Air Mouse BLE");
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(10, H / 2 - 16);
        M5.Display.print(s_connected ? "Connected" : "Advertising...");
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, H - 12);
        M5.Display.print("A=LMB B=RMB A+B=exit");
        k85_draw_battery_icon();
    };

    draw_status();
    bool last_connected = s_connected;

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();

            // Полностью гасим BT перед восстановлением WiFi — иначе BT-стек
            // продолжает держать internal RAM, и повторный k85_wifi_init()
            // падает.
            esp_bluedroid_disable();
            esp_bluedroid_deinit();
            esp_bt_controller_disable();
            esp_bt_controller_deinit();
            s_hid_inited = false;

            if (!g_config.wifi_disabled) k85_wifi_init();
            return;
        }
        if (s_connected != last_connected) {
            last_connected = s_connected;
            draw_status();
        }

        if (s_connected) {
            float ax = 0, ay = 0, az = 0;
            M5.Imu.getAccel(&ax, &ay, &az);

            int dx = (int)(-ax * 20.0f);
            int dy = (int)(ay * 20.0f);
            if (dx > 127) dx = 127;
            if (dx < -127) dx = -127;
            if (dy > 127) dy = 127;
            if (dy < -127) dy = -127;

            uint8_t buttons = 0;
            if (k85_btn_a_pressed() || M5.BtnA.isPressed()) buttons |= 0x01; // LMB
            if (k85_btn_b_pressed() || M5.BtnB.isPressed()) buttons |= 0x02; // RMB

            if (dx != 0 || dy != 0 || buttons != 0) {
                esp_hidd_send_mouse_value(s_conn_id, buttons, (int8_t)dx, (int8_t)dy);
                k85_wake_screen();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}


