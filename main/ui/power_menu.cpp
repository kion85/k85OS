#include "power_menu.h"
#include "list_menu.h"
#include "power.h"
#include "config.h"
#include "common.h"
#include "input.h"
#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

// Иконки для Power-подменю (grid/list+icons режимы).
static void draw_power_icon(int cx, int cy, int r, const char *name, uint32_t col, uint32_t bg_col) {
    auto &d = M5.Display;
    if (!strncmp(name, "Sleep now", 9)) {
        // Полумесяц: круг фона поверх смещённого круга цвета иконки.
        d.fillCircle(cx, cy, r, col);
        d.fillCircle(cx + r/2, cy - r/3, r, bg_col);
    } else if (!strncmp(name, "Sleep wake", 10)) {
        // Иконка "палец нажимает кнопку" + стрелка (пробуждение).
        d.drawRoundRect(cx - r/2, cy - r, r, r * 2, r/4, col);
        d.fillCircle(cx, cy - r/2, 2, col);
        d.drawLine(cx + r/2, cy, cx + r, cy, col);
        d.drawLine(cx + r/2, cy - 3, cx + r, cy, col);
        d.drawLine(cx + r/2, cy + 3, cx + r, cy, col);
    } else if (!strcmp(name, "Power Off")) {
        d.drawCircle(cx, cy, r, col);
        d.drawLine(cx, cy - r, cx, cy - r/3, col);
        // Разрыв окружности сверху (классический символ питания) -
        // рисуем фоном поверх верхней дуги.
        d.drawFastHLine(cx - 2, cy - r, 4, bg_col);
    } else if (!strcmp(name, "Reboot")) {
        d.drawArc(cx, cy, r/2, r, 40, 320, col);
        d.fillTriangle(cx + r - 2, cy - r/3, cx + r + 3, cy - r/3, cx + r, cy - r/3 - 5, col);
    } else if (!strcmp(name, "Back")) {
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy, col);
        d.drawLine(cx - r/2, cy, cx + r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy, cx + r, cy, col);
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
}

// Простое да/нет подтверждение для необратимых действий (Power Off/Reboot),
// в том же стиле, что confirm-диалоги в settings_menu.cpp (SSH disable и т.п.).
static bool confirm_action(const char *message) {
    k85_show_message(message);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return false;
        }
        if (k85_btn_b_pressed()) return true;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_run_power_menu(void) {
    while (true) {
        int wake_mode = g_config.sleep_wake_mode;
        if (wake_mode < 0 || wake_mode > 2) wake_mode = 0;

        char wake_label[40];
        snprintf(wake_label, sizeof(wake_label), "Sleep wake: %s", k85_sleep_wake_mode_names[wake_mode]);

        const char *items[] = { "Sleep now", wake_label, "Power Off", "Reboot", "Back" };
        const int count = (int)(sizeof(items) / sizeof(items[0]));

        int idx = k85_run_list_menu("POWER", items, count, nullptr, draw_power_icon);
        if (idx < 0 || idx == count - 1) return;

        if (idx == 0) {
            k85_enter_sleep();
        } else if (idx == 1) {
            g_config.sleep_wake_mode = (wake_mode + 1) % 3;
            k85_config_save();
        } else if (idx == 2) {
            if (confirm_action("Power off device?\nB=confirm A+B=cancel")) {
                k85_power_off(); // не возвращается
            }
        } else if (idx == 3) {
            if (confirm_action("Reboot device?\nB=confirm A+B=cancel")) {
                k85_power_reboot(); // не возвращается
            }
        }
    }
}