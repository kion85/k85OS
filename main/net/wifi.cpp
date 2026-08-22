#include "wifi.h"
#include "config.h"
#include "log.h"
#include "notifications.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

static EventGroupHandle_t s_wifi_event_group = nullptr;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT      = BIT1;

static bool s_inited = false;         // драйвер сейчас активен (между init и stop/deinit)
static bool s_netif_created = false;  // netif/event loop создаются один раз за всё время жизни прошивки
static bool s_connected = false;
static esp_netif_t *s_netif = nullptr;

static esp_event_handler_instance_t s_wifi_evt_instance = nullptr;
static esp_event_handler_instance_t s_ip_evt_instance = nullptr;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was_connected = s_connected;
        s_connected = false;
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        k85_log("WiFi disconnected");
        if (was_connected) k85_notify("WiFi disconnected");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        k85_log("WiFi got IP");
        k85_notify("WiFi connected");
    }
}

void k85_wifi_init(void) {
    if (s_inited) return;

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }

    // netif и дефолтный event loop создаются один раз за всё время жизни
    // прошивки — пересоздавать их на каждый init/deinit цикл нельзя (это
    // не то, что реально освобождает esp_wifi_deinit(), а отдельные
    // сущности, которые просто незачем плодить повторно).
    if (!s_netif_created) {
        esp_netif_init();
        esp_event_loop_create_default();
        s_netif = esp_netif_create_default_wifi_sta();
        s_netif_created = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // Регистрируем обработчики событий только если их ещё нет (после
    // k85_wifi_stop() они снимаются и обнуляются — иначе тут задвоились бы
    // при каждом повторном включении WiFi).
    if (!s_wifi_evt_instance) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, nullptr, &s_wifi_evt_instance);
    }
    if (!s_ip_evt_instance) {
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler, nullptr, &s_ip_evt_instance);
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    s_inited = true;
}

// общая функция подключения, используется и в connect_saved, и в connect
static bool wifi_do_connect(const char *ssid, const char *password) {
    if (!s_inited) k85_wifi_init();
    if (!s_wifi_event_group) s_wifi_event_group = xEventGroupCreate();
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool k85_wifi_connect_saved(void) {
    if (!g_config.wifi_saved || strlen(g_config.wifi_ssid) == 0) {
        k85_log("No saved WiFi network");
        return false;
    }
    return wifi_do_connect(g_config.wifi_ssid, g_config.wifi_password);
}

bool k85_wifi_connect(const char *ssid, const char *password) {
    return wifi_do_connect(ssid, password);
}

int k85_wifi_scan(char ssids_out[][33], int max_results) {
    if (!s_inited) k85_wifi_init();

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan_config, true); // блокирующее сканирование
    if (err != ESP_OK) {
        k85_log("WiFi scan failed");
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return 0;

    wifi_ap_record_t *records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!records) return 0;
    esp_wifi_scan_get_ap_records(&ap_count, records);

    int n = 0;
    for (int i = 0; i < ap_count && n < max_results; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == 0) continue;
        // отфильтровываем дубликаты SSID (несколько точек с одной сетью)
        bool dup = false;
        for (int j = 0; j < n; j++) if (!strcmp(ssids_out[j], ssid)) { dup = true; break; }
        if (dup) continue;
        snprintf(ssids_out[n], 33, "%s", ssid);
        n++;
    }
    free(records);
    return n;
}

void k85_wifi_disconnect(void) {
    esp_wifi_disconnect();
    s_connected = false;
}

// Полная остановка И деинициализация драйвера WiFi. esp_wifi_stop() САМ ПО
// СЕБЕ НЕ освобождает internal RAM — драйвер держит свои буферы вплоть до
// esp_wifi_deinit(). Именно это было причиной, почему BLE всё равно падал
// с "Malloc failed" даже после k85_wifi_stop(): драйвер был остановлен, но
// память так и не освободилась. Снимаем также обработчики событий, чтобы
// следующий k85_wifi_init() не зарегистрировал их повторно поверх старых.
void k85_wifi_stop(void) {
    if (!s_inited) return;

    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_wifi_evt_instance) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt_instance);
        s_wifi_evt_instance = nullptr;
    }
    if (s_ip_evt_instance) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt_instance);
        s_ip_evt_instance = nullptr;
    }

    s_connected = false;
    s_inited = false;
}

bool k85_wifi_is_connected(void) { return s_connected; }

const char *k85_wifi_get_ip_str(void) {
    static char buf[24] = "";
    if (!s_connected || !s_netif) {
        buf[0] = 0;
        return buf;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_netif, &ip_info) == ESP_OK) {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
    } else {
        buf[0] = 0;
    }
    return buf;
}

int k85_wifi_get_rssi(void) {
    if (!s_connected) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}