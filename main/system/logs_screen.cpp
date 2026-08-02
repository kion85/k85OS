#include "logs_screen.h"
#include "theme.h"
#include "power.h"
#include "input.h"
#include "log.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void render(int offset) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    int count = k85_log_count();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(4, 4);
    M5.Display.print("System log:");

    int y = 18;
    for (int i = offset; i < offset + 8 && i < count; i++) {
        M5.Display.setCursor(4, y);
        M5.Display.print(k85_log_get(i));
        y += 12;
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(4, M5.Display.height() - 12);
    M5.Display.print("A=scroll A+B=exit");
}

// Аналог run_logs() из MicroPython
void k85_run_logs_screen(void) {
    int offset = 0;
    render(offset);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            int count = k85_log_count();
            offset = (offset + 8) % (count > 0 ? count : 1);
            render(offset);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}