#include "ftp_tool.h"
#include "common.h"
#include "input.h"
#include "list_menu.h"
#include "../../core/svc_cfg.h"
#include "../../net/ftp_server.h"
#include "../../net/wifi.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

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

static void toggle_server(void) {
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

static void show_info(void) {
    if (!k85_ftp_server_is_running()) {
        show_and_wait("FTP server stopped\nStart it first\nA+B=back");
        return;
    }
    char user[40] = {0};
    k85_svc_get_str("ftp_user", user, sizeof(user));
    char msg[160];
    snprintf(msg, sizeof(msg), "ftp://%s:21\nuser: %s\npassive mode\nA+B=back", k85_wifi_get_ip_str(), user);
    show_and_wait(msg);
}

void k85_run_ftp_tool(void) {
    while (true) {
        bool running = k85_ftp_server_is_running();
        bool autostart = k85_svc_get_bool("ftp_auto", false);

        char l_toggle[24], l_auto[32], header[32];
        snprintf(l_toggle, sizeof(l_toggle), "%s", running ? "Stop server" : "Start server");
        snprintf(l_auto, sizeof(l_auto), "Autostart: %s", autostart ? "ON" : "OFF");
        snprintf(header, sizeof(header), "FTP SERVER (%s)", running ? "ON" : "OFF");

        const char *items[] = { l_toggle, "Connection info", l_auto, "Back" };
        int idx = k85_run_list_menu(header, items, 4, nullptr);
        if (idx < 0 || idx == 3) return;

        if (idx == 0) {
            toggle_server();
        } else if (idx == 1) {
            show_info();
        } else if (idx == 2) {
            bool now_on = !autostart;
            k85_svc_set_bool("ftp_auto", now_on);
            if (now_on && !k85_ftp_has_login()) {
                show_and_wait("FTP login not set.\nIn Terminal run:\nftppass <user> <pass>\nA+B=back");
            }
        }
    }
}