#pragma once
#include <stdbool.h>

// Инициализация netif+event loop+esp_wifi (вызывать раз при старте, после nvs_flash_init())
void k85_wifi_init(void);

// Пытается подключиться к сети из config.wifi (ssid/password), таймаут ~8с, блокирующая
bool k85_wifi_connect_saved(void);

// Подключение к произвольным ssid/password, таймаут ~8с, блокирующая.
// При успехе НЕ сохраняет в конфиг сама — сохранение делает вызывающий код (UI),
// чтобы backend не решал за UI, когда персистить.
bool k85_wifi_connect(const char *ssid, const char *password);

// Блокирующий скан. Пишет до max_results SSID в ssids_out (каждый до 32 символов + \0),
// возвращает реальное количество найденных (может быть меньше max_results).
int k85_wifi_scan(char ssids_out[][33], int max_results);

// Отключение / выключение радио
void k85_wifi_disconnect(void);

bool k85_wifi_is_connected(void);

// "192.168.1.23" или "" если не подключены
const char *k85_wifi_get_ip_str(void);

// RSSI текущего подключения, 0 если не подключены
int k85_wifi_get_rssi(void);
