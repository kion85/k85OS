#include "lock_screen_settings.h"
#include "common.h"
#include "theme.h"
#include "input.h"
#include "list_menu.h"
#include "../core/config.h"
#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

static const uint32_t k85_lock_color_presets[] = {
    0xFFFFFFFF, // авто-палитра (как сейчас)
    0xFFFFFF, 0xFF6699, 0x66FFCC, 0xFFCC66, 0x9966FF, 0x00FFFF,
};
#define K85_LOCK_COLOR_PRESET_COUNT (int)(sizeof(k85_lock_color_presets) / sizeof(k85_lock_color_presets[0]))

// Иконки: подписи динамические ("Shape: Circle" и т.п.), поэтому сравниваем
// через strncmp по префиксу.
static void draw_lock_settings_icon(int cx, int cy, int r, const char *name, uint32_t col, uint32_t bg_col) {
    auto &d = M5.Display;
    if (!strncmp(name, "Shape", 5)) {
        d.drawCircle(cx - r/3, cy, r/2, col);
        d.drawRect(cx, cy - r/2, r, r, col);
    } else if (!strncmp(name, "Color", 5)) {
        d.fillCircle(cx, cy, r, col);
        d.fillArc(cx, cy, 0, r, 0, 180, bg_col);
    } else if (!strcmp(name, "Back")) {
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy, col);
        d.drawLine(cx - r/2, cy, cx + r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy, cx + r, cy, col);
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
}

void k85_run_lock_screen_settings(void) {
    static const char *shape_names[3] = {"Circle", "Square", "Mixed"};

    while (true) {
        int s = g_config.lock_shape;
        if (s < 0 || s > 2) s = 0;

        int color_idx = 0;
        for (int p = 0; p < K85_LOCK_COLOR_PRESET_COUNT; p++) {
            if (k85_lock_color_presets[p] == g_config.lock_particle_color) { color_idx = p; break; }
        }

        char shape_label[32], color_label[32];
        snprintf(shape_label, sizeof(shape_label), "Shape: %s", shape_names[s]);
        snprintf(color_label, sizeof(color_label), "Color: %s", color_idx == 0 ? "Auto" : "Custom");

        const char *items[] = { shape_label, color_label, "Back" };
        int idx = k85_run_list_menu("LOCK SCREEN PARTICLES", items, 3, nullptr, draw_lock_settings_icon);
        if (idx < 0 || idx == 2) return;

        if (idx == 0) {
            g_config.lock_shape = (s + 1) % 3;
        } else if (idx == 1) {
            int new_idx = (color_idx + 1) % K85_LOCK_COLOR_PRESET_COUNT;
            g_config.lock_particle_color = k85_lock_color_presets[new_idx];
        }
    }
}