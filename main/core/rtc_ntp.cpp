#include "rtc_ntp.h"
#include "log.h"

#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

static bool s_ntp_synced = false;
static int64_t s_last_ntp_sync_us = 0;

void k85_rtc_ntp_init(void) {
    setenv("TZ", "MSK-3", 1);
    tzset();
}

bool k85_ntp_sync_now(void) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t init_err = esp_netif_sntp_init(&config);
    if (init_err != ESP_OK) {
        k85_log("NTP init failed");
        return false;
    }
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000));
    esp_netif_sntp_deinit();

    if (err == ESP_OK) {
        s_ntp_synced = true;
        s_last_ntp_sync_us = esp_timer_get_time();
        k85_log("NTP synced");
        return true;
    }
    k85_log("NTP sync failed (no network?)");
    return false;
}

bool k85_is_ntp_synced(void) { return s_ntp_synced; }

const char *k85_get_uptime_str(void) {
    static char buf[24];
    int64_t s = esp_timer_get_time() / 1000000;
    int h = (int)(s / 3600);
    int m = (int)((s % 3600) / 60);
    int sec = (int)(s % 60);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, sec);
    return buf;
}

const char *k85_get_time_str(void) {
    static char buf[24];

    if (s_ntp_synced && (esp_timer_get_time() - s_last_ntp_sync_us) > 3600000000LL) {
        s_ntp_synced = false;
    }

    if (s_ntp_synced) {
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return buf;
    }
    return k85_get_uptime_str();
}

const char *k85_get_date_str(void) {
    static char buf[40];
    if (s_ntp_synced) {
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        snprintf(buf, sizeof(buf), "%02d.%02d.%04d",
                 timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
        return buf;
    }
    snprintf(buf, sizeof(buf), "--.--.----");
    return buf;
}
