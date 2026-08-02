#include "list_menu.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "config.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>

int k85_run_list_menu(const char *title, const char *const items[], int count,
                       const char *const score_keys[]) {
    if (count <= 0) return -1;
    int sel = 0;
    int scroll = 0;

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int H = M5.Display.height();

    while (true) {
        if (sel >= count) sel = count - 1;

        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.print(title);

        M5.Display.setTextSize(2);
        const int line_h = 24;
        const int start_y = 16;
        int visible = (H - start_y) / line_h;
        if (visible < 1) visible = 1;

        if (sel < scroll) scroll = sel;
        else if (sel >= scroll + visible) scroll = sel - visible + 1;

        int end = count < (scroll + visible) ? count : (scroll + visible);

        for (int i = scroll; i < end; i++) {
            char label[64];
            if (score_keys && score_keys[i]) {
                snprintf(label, sizeof(label), "%s (%d)", items[i], k85_get_high_score(score_keys[i]));
            } else {
                snprintf(label, sizeof(label), "%s", items[i]);
            }
            int yy = start_y + (i - scroll) * line_h;
            if (i == sel) {
                M5.Display.setTextColor(0x000000, accent);
                M5.Display.setCursor(4, yy);
                M5.Display.printf(">%s", label);
            } else {
                M5.Display.setTextColor(fg, bg);
                M5.Display.setCursor(4, yy);
                M5.Display.printf(" %s", label);
            }
        }
        k85_draw_battery_icon();

        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) {
                k85_wait_ab_release();
                return -1;
            }
            if (k85_btn_a_pressed()) {
                k85_wake_screen();
                sel = (sel + 1) % count;
                break;
            }
            if (k85_btn_b_pressed()) {
                k85_wake_screen();
                if (!strcmp(items[sel], "Back")) return -1;
                return sel;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}
