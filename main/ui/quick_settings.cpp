#include "ui/quick_settings.h"
#include "M5Unified.h"
#include "core/config.h"
#include "core/theme.h"
#include "core/input.h"
#include "core/sound.h"
#include "net/wifi.h"
#include "ui/common.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>

#define K85_QS_ITEM_COUNT 5

enum {
    QS_BRIGHTNESS = 0,
    QS_WIFI,
    QS_BLUETOOTH,
    QS_FLASHLIGHT,
    QS_SOUND,
};

static int s_qs_selected = 0;
static int s_muted_prev_volume = -1; // -1 = звук не в mute

static const int BRIGHTNESS_LEVELS[] = {25, 50, 75, 100};
#define BRIGHTNESS_LEVEL_COUNT (int)(sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]))

static void qs_run_flashlight(void) {
    M5.Display.fillScreen(0xFFFFFF);
    M5.Display.setBrightness(100);
    k85_wait_ab_release();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            M5.Display.setBrightness(g_config.brightness_active);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void qs_cycle_brightness(void) {
    int cur = g_config.brightness_active;
    int next = BRIGHTNESS_LEVELS[0];
    bool found = false;
    for (int i = 0; i < BRIGHTNESS_LEVEL_COUNT; i++) {
        if (BRIGHTNESS_LEVELS[i] > cur) {
            next = BRIGHTNESS_LEVELS[i];
            found = true;
            break;
        }
    }
    if (!found) next = BRIGHTNESS_LEVELS[0]; // дошли до конца — на первый уровень
    g_config.brightness_active = next;
    M5.Display.setBrightness(next);
    k85_config_save();
}

static void qs_toggle_wifi(void) {
    if (k85_wifi_is_connected()) {
        k85_wifi_disconnect();
        return;
    }
    if (!g_config.wifi_saved || g_config.wifi_ssid[0] == '\0') {
        k85_show_message("No saved WiFi");
        vTaskDelay(pdMS_TO_TICKS(800));
        return;
    }
    k85_show_message("Connecting...");
    k85_wifi_connect_saved();
}

static void qs_toggle_sound(void) {
    if (s_muted_prev_volume >= 0) {
        k85_set_sound_volume(s_muted_prev_volume);
        s_muted_prev_volume = -1;
    } else {
        int cur = k85_get_sound_volume();
        if (cur > 0) {
            s_muted_prev_volume = cur;
            k85_set_sound_volume(0);
        }
    }
}

static void qs_activate_selected(void) {
    switch (s_qs_selected) {
        case QS_BRIGHTNESS: qs_cycle_brightness(); break;
        case QS_WIFI:       qs_toggle_wifi();      break;
        case QS_BLUETOOTH:  /* индикатор, без действия */ break;
        case QS_FLASHLIGHT: qs_run_flashlight();   break;
        case QS_SOUND:      qs_toggle_sound();     break;
    }
}

static void qs_draw(void) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    int w = M5.Display.width();
    int h = M5.Display.height();
    int panel_h = h * 2 / 3;
    int y0 = h - panel_h;

    M5.Display.fillRect(0, y0, w, panel_h, bg);
    M5.Display.drawRect(0, y0, w, panel_h, accent);

    int row_h = panel_h / K85_QS_ITEM_COUNT;
    M5.Display.setTextSize(2);

    char line[48];
    for (int i = 0; i < K85_QS_ITEM_COUNT; i++) {
        int ry = y0 + i * row_h;
        bool sel = (i == s_qs_selected);
        M5.Display.fillRect(2, ry + 1, w - 4, row_h - 2, sel ? accent : bg);
        M5.Display.setTextColor(sel ? bg : fg, sel ? accent : bg);
        M5.Display.setCursor(8, ry + row_h / 2 - 8);

        switch (i) {
            case QS_BRIGHTNESS:
                snprintf(line, sizeof(line), "Brightness: %d%%", g_config.brightness_active);
                break;
            case QS_WIFI:
                snprintf(line, sizeof(line), "WiFi: %s", k85_wifi_is_connected() ? "Connected" : "Off");
                break;
            case QS_BLUETOOTH:
                snprintf(line, sizeof(line), "Bluetooth: HID ready");
                break;
            case QS_FLASHLIGHT:
                snprintf(line, sizeof(line), "Flashlight");
                break;
            case QS_SOUND:
                snprintf(line, sizeof(line), "Sound: %s", s_muted_prev_volume >= 0 ? "Muted" : "On");
                break;
        }
        M5.Display.print(line);
    }
}

void k85_quick_settings_open(void) {
    s_qs_selected = 0;
    k85_wait_ab_release();
    qs_draw();

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }

        if (k85_btn_a_pressed()) {
            s_qs_selected = (s_qs_selected + 1) % K85_QS_ITEM_COUNT;
            qs_draw();
        }

        if (k85_btn_b_pressed()) {
            qs_activate_selected();
            qs_draw();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
