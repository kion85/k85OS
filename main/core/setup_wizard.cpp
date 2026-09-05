#include "setup_wizard.h"
#include "config.h"
#include "theme.h"
#include "device.h"
#include "rtc_ntp.h"
#include "../net/wifi.h"
#include "list_menu.h"
#include "text_input.h"
#include "common.h"
#include "input.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

enum K85SetupResult { K85_SETUP_CONTINUE, K85_SETUP_SKIP_ALL };

static void wait_ab_back(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void show_welcome_animation(void) {
    int W = M5.Display.width();
    int H = M5.Display.height();

    M5.Display.fillScreen(0x000000);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(0x00FFFF, 0x000000);
    const char *title = "k85OS";
    int tw = (int)strlen(title) * 18;
    M5.Display.setCursor((W - tw) / 2, H / 2 - 40);
    M5.Display.print(title);
    vTaskDelay(pdMS_TO_TICKS(900));

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(0xFFFFFF, 0x000000);
    const char *welcome = "Welcome";
    int ww = (int)strlen(welcome) * 12;
    M5.Display.setCursor((W - ww) / 2, H / 2 + 10);
    M5.Display.print(welcome);
    vTaskDelay(pdMS_TO_TICKS(1200));

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x888888, 0x000000);
    const char *sub = "Let's set things up";
    int sw = (int)strlen(sub) * 6;
    M5.Display.setCursor((W - sw) / 2, H / 2 + 34);
    M5.Display.print(sub);
    vTaskDelay(pdMS_TO_TICKS(1200));
}

// ---------- Шаг Wi-Fi ----------
static K85SetupResult step_wifi(void) {
    while (true) {
        const char *items[] = { "Scan & connect", "Skip", "Skip all" };
        int idx = k85_run_list_menu("SETUP: WiFi", items, 3, nullptr);
        if (idx < 0 || idx == 1) return K85_SETUP_CONTINUE;
        if (idx == 2) return K85_SETUP_SKIP_ALL;

        k85_show_message("Scanning...");
        char ssids[16][33];
        int n = k85_wifi_scan(ssids, 16);
        if (n == 0) {
            k85_show_message("No networks found\nA+B=back");
            wait_ab_back();
            continue;
        }

        const char *net_items[17];
        for (int i = 0; i < n; i++) net_items[i] = ssids[i];
        net_items[n] = "Back";
        int sidx = k85_run_list_menu("WIFI NETWORKS", net_items, n + 1, nullptr);
        if (sidx < 0 || sidx == n) continue;

        char password[64] = "";
        if (!k85_text_input("Enter password:", "", password, sizeof(password))) continue;

        k85_show_message("Connecting...");
        bool ok = k85_wifi_connect(ssids[sidx], password);
        if (ok) {
            snprintf(g_config.wifi_ssid, sizeof(g_config.wifi_ssid), "%s", ssids[sidx]);
            snprintf(g_config.wifi_password, sizeof(g_config.wifi_password), "%s", password);
            g_config.wifi_saved = true;
            k85_config_save();
            k85_show_message("Connected!");
            vTaskDelay(pdMS_TO_TICKS(1200));
            return K85_SETUP_CONTINUE;
        } else {
            k85_show_message("Connect failed\n(timeout)");
            vTaskDelay(pdMS_TO_TICKS(1200));
        }
    }
}

// ---------- Шаг темы ----------
static K85SetupResult step_theme(void) {
    int count = k85_theme_count();
    if (count > 30) count = 30;
    const char *items[32];
    for (int i = 0; i < count; i++) items[i] = k85_get_theme_by_index(i)->name;
    items[count] = "Skip";
    items[count + 1] = "Skip all";

    int idx = k85_run_list_menu("SETUP: Theme", items, count + 2, nullptr);
    if (idx < 0 || idx == count) return K85_SETUP_CONTINUE;
    if (idx == count + 1) return K85_SETUP_SKIP_ALL;

    g_config.theme_idx = idx;
    k85_config_save();
    return K85_SETUP_CONTINUE;
}

// ---------- Шаг времени/часового пояса ----------
static K85SetupResult step_time(void) {
    const char *items[] = { "Set time zone", "Skip", "Skip all" };
    int idx = k85_run_list_menu("SETUP: Time", items, 3, nullptr);
    if (idx < 0 || idx == 1) return K85_SETUP_CONTINUE;
    if (idx == 2) return K85_SETUP_SKIP_ALL;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", g_config.utc_offset);
    if (k85_text_input("UTC offset (-12..12):", buf, buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v < -12) v = -12;
        if (v > 12) v = 12;
        g_config.utc_offset = v;
        k85_rtc_apply_tz(v);
        k85_config_save();

        if (k85_wifi_is_connected()) {
            k85_show_message("Syncing time...");
            k85_ntp_sync_now();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    return K85_SETUP_CONTINUE;
}

// ---------- Шаг имени устройства ----------
static K85SetupResult step_device_name(void) {
    const char *items[K85_DEVICE_NAME_COUNT + 2];
    for (int i = 0; i < K85_DEVICE_NAME_COUNT; i++) items[i] = k85_device_names[i];
    items[K85_DEVICE_NAME_COUNT] = "Skip";
    items[K85_DEVICE_NAME_COUNT + 1] = "Skip all";

    int idx = k85_run_list_menu("SETUP: Device name", items, K85_DEVICE_NAME_COUNT + 2, nullptr);
    if (idx < 0 || idx == K85_DEVICE_NAME_COUNT) return K85_SETUP_CONTINUE;
    if (idx == K85_DEVICE_NAME_COUNT + 1) return K85_SETUP_SKIP_ALL;

    g_config.device_name_idx = idx;
    k85_config_save();
    return K85_SETUP_CONTINUE;
}

void k85_run_setup_wizard(void) {
    show_welcome_animation();

    bool skip_all = false;
    if (!skip_all && step_wifi() == K85_SETUP_SKIP_ALL) skip_all = true;
    if (!skip_all && step_theme() == K85_SETUP_SKIP_ALL) skip_all = true;
    if (!skip_all && step_time() == K85_SETUP_SKIP_ALL) skip_all = true;
    if (!skip_all && step_device_name() == K85_SETUP_SKIP_ALL) skip_all = true;

    g_config.setup_completed = true;
    k85_config_save();

    M5.Display.fillScreen(0x000000);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(0x00FF00, 0x000000);
    M5.Display.setCursor(20, M5.Display.height() / 2 - 10);
    M5.Display.print("All set!");
    vTaskDelay(pdMS_TO_TICKS(1200));
}