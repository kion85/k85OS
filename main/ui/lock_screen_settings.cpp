#include "lock_screen_settings.h"
#include "common.h"
#include "theme.h"
#include "input.h"
#include "../core/config.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

static const uint32_t k85_lock_color_presets[] = {
    0xFFFFFFFF, // авто-палитра (как сейчас)
    0xFFFFFF, 0xFF6699, 0x66FFCC, 0xFFCC66, 0x9966FF, 0x00FFFF,
};
#define K85_LOCK_COLOR_PRESET_COUNT (int)(sizeof(k85_lock_color_presets) / sizeof(k85_lock_color_presets[0]))

void k85_run_lock_screen_settings(void) {
    static const char *shape_names[3] = {"Circle", "Square", "Mixed"};
    int selected = 0;
    uint32_t bg = k85_get_bg();

    while (true) {
        k85_input_update();
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(k85_get_fg(), bg);
        M5.Display.setCursor(6, 6);
        M5.Display.print("Lock screen particles");

        char val[20];
        for (int i = 0; i < 3; i++) {
            bool sel = (i == selected);
            M5.Display.setTextColor(sel ? k85_get_accent() : k85_get_fg(), bg);
            M5.Display.setCursor(6, 24 + i * 16);
            M5.Display.print(sel ? "> " : "  ");

            if (i == 0) {
                M5.Display.print("Shape");
                int s = g_config.lock_shape;
                if (s < 0 || s > 2) s = 0;
                snprintf(val, sizeof(val), "%s", shape_names[s]);
                M5.Display.setCursor(140, 24 + i * 16);
                M5.Display.print(val);
            } else if (i == 1) {
                M5.Display.print("Color");
                int idx = 0;
                for (int p = 0; p < K85_LOCK_COLOR_PRESET_COUNT; p++) {
                    if (k85_lock_color_presets[p] == g_config.lock_particle_color) { idx = p; break; }
                }
                snprintf(val, sizeof(val), "%s", idx == 0 ? "Auto" : "Custom");
                M5.Display.setCursor(140, 24 + i * 16);
                M5.Display.print(val);
            } else {
                M5.Display.print("Back");
            }
        }

        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(6, 100);
        M5.Display.print("A=next B=change A+B=back");

        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed()) { selected = (selected + 1) % 3; }
        if (k85_btn_b_pressed() && !k85_btn_a_is_down()) {
            if (selected == 0) {
                g_config.lock_shape = (g_config.lock_shape + 1) % 3;
            } else if (selected == 1) {
                int idx = 0;
                for (int p = 0; p < K85_LOCK_COLOR_PRESET_COUNT; p++) {
                    if (k85_lock_color_presets[p] == g_config.lock_particle_color) { idx = p; break; }
                }
                idx = (idx + 1) % K85_LOCK_COLOR_PRESET_COUNT;
                g_config.lock_particle_color = k85_lock_color_presets[idx];
            } else {
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

