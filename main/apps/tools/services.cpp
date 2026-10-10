#include "services.h"
#include "common.h"
#include "input.h"
#include "list_menu.h"
#include "../../core/config.h"
#include "../../core/cron.h"
#include "../../core/svc_cfg.h"
#include "../../net/ssh_server.h"
#include "../../net/ftp_server.h"
#include "../../net/wifi.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

#define K85_SVC_ITEM_COUNT 8

static const char *k85_svc_labels[K85_SVC_ITEM_COUNT] = {
    "WiFi Hotspot autostart", "MQTT autostart", "OTA background check",
    "FTP Server", "FTP autostart", "Startup script", "Scheduler (cron)", "Back",
};

static void svc_value_str(char *out, size_t out_size, int idx) {
    switch (idx) {
        case 0: snprintf(out, out_size, "N/A"); break;
        case 1: snprintf(out, out_size, "%s", g_config.mqtt_autostart ? "ON" : "OFF"); break;
        case 2: snprintf(out, out_size, "%s", g_config.ota_bg_check_enabled ? "ON" : "OFF"); break;
        case 3: snprintf(out, out_size, "%s", k85_ftp_server_is_running() ? "ON" : "OFF"); break;
        case 4: snprintf(out, out_size, "%s", k85_svc_get_bool("ftp_auto", false) ? "ON" : "OFF"); break;
        case 5: snprintf(out, out_size, "%s", k85_svc_get_bool("startup", true) ? "ON" : "OFF"); break;
        case 6: snprintf(out, out_size, "%s", k85_cron_is_running() ? "ON" : "OFF"); break;
        default: out[0] = 0;
    }
}

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void show_and_wait(const char *msg) {
    k85_show_message(msg);
    wait_ab_exit();
}

static void toggle_ftp_server(void) {
    if (k85_ftp_server_is_running()) {
        k85_show_message("Stopping FTP...");
        k85_ftp_server_stop();
        return;
    }
    switch (k85_ftp_server_start(true)) {
        case K85_FTP_START_OK: {
            char msg[128];
            snprintf(msg, sizeof(msg), "FTP started\nftp://%s\nstops after 15 min idle\nA+B=back", k85_wifi_get_ip_str());
            show_and_wait(msg);
            break;
        }
        case K85_FTP_START_NO_WIFI:
            show_and_wait("WiFi not connected\nA+B=back");
            break;
        case K85_FTP_START_NO_LOGIN:
            show_and_wait("FTP login not set.\nIn Terminal run:\nftppass <user> <pass>\nA+B=back");
            break;
        case K85_FTP_START_ALREADY_RUNNING:
            break;
        default:
            show_and_wait("FTP failed to start\n(port busy or\nout of memory)\nA+B=back");
            break;
    }
}

void k85_run_services(void) {
    while (true) {
        char labeled[K85_SVC_ITEM_COUNT][56];
        const char *items[K85_SVC_ITEM_COUNT];
        for (int i = 0; i < K85_SVC_ITEM_COUNT; i++) {
            char val[16];
            svc_value_str(val, sizeof(val), i);
            if (val[0]) snprintf(labeled[i], sizeof(labeled[i]), "%s: %s", k85_svc_labels[i], val);
            else snprintf(labeled[i], sizeof(labeled[i]), "%s", k85_svc_labels[i]);
            items[i] = labeled[i];
        }

        int idx = k85_run_list_menu("SERVICES", items, K85_SVC_ITEM_COUNT, nullptr);
        if (idx < 0 || idx == K85_SVC_ITEM_COUNT - 1) return;

        if (idx == 0) {
            show_and_wait("Hotspot autostart\nnot yet available\nA+B=back");
        } else if (idx == 1) {
            g_config.mqtt_autostart = !g_config.mqtt_autostart;
            k85_config_save();
        } else if (idx == 2) {
            g_config.ota_bg_check_enabled = !g_config.ota_bg_check_enabled;
            k85_config_save();
        } else if (idx == 3) {
            toggle_ftp_server();
        } else if (idx == 4) {
            bool now_on = !k85_svc_get_bool("ftp_auto", false);
            k85_svc_set_bool("ftp_auto", now_on);
            if (now_on && !k85_ftp_has_login()) {
                show_and_wait("FTP login not set.\nIn Terminal run:\nftppass <user> <pass>\nA+B=back");
            }
        } else if (idx == 5) {
            k85_svc_set_bool("startup", !k85_svc_get_bool("startup", true));
        } else if (idx == 6) {
            if (k85_cron_is_running()) {
                k85_svc_set_bool("cron", false);
                k85_cron_stop();
            } else {
                k85_svc_set_bool("cron", true);
                if (!k85_cron_start()) show_and_wait("Scheduler failed\nto start\nA+B=back");
            }
        }
    }
}

// K85_SSH_REMOVED
