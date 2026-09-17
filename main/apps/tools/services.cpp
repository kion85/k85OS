#include "services.h"
#include "common.h"
#include "input.h"
#include "list_menu.h"
#include "../../core/config.h"
#include "../../net/ssh_server.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

#define K85_SVC_ITEM_COUNT 5

static const char *k85_svc_labels[K85_SVC_ITEM_COUNT] = {
    "SSH Server", "WiFi Hotspot autostart", "MQTT autostart", "OTA background check", "Back",
};

static void svc_value_str(char *out, size_t out_size, int idx) {
    switch (idx) {
        case 0: snprintf(out, out_size, "%s", g_config.ssh_enabled ? "ON" : "OFF"); break;
        case 1: snprintf(out, out_size, "N/A"); break;
        case 2: snprintf(out, out_size, "%s", g_config.mqtt_autostart ? "ON" : "OFF"); break;
        case 3: snprintf(out, out_size, "%s", g_config.ota_bg_check_enabled ? "ON" : "OFF"); break;
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
            if (g_config.ssh_enabled) {
                g_config.ssh_enabled = false;
                k85_ssh_server_stop();
                k85_config_save();
            } else {
                k85_show_message("Set up SSH in BIOS\n(needs username/\npassword)\nA+B=back");
                wait_ab_exit();
            }
        } else if (idx == 1) {
            k85_show_message("Hotspot autostart\nnot yet available\nA+B=back");
            wait_ab_exit();
        } else if (idx == 2) {
            g_config.mqtt_autostart = !g_config.mqtt_autostart;
            k85_config_save();
        } else if (idx == 3) {
            g_config.ota_bg_check_enabled = !g_config.ota_bg_check_enabled;
            k85_config_save();
        }
    }
}
