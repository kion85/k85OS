#include "color_test.h"
#include "power.h"
#include "input.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct ColorEntry { const char *name; uint32_t val; };

// Та же последовательность, что и colors_seq в оригинале
static const ColorEntry COLORS_SEQ[] = {
    {"RED", 0xFF0000}, {"GREEN", 0x00FF00}, {"BLUE", 0x0000FF},
    {"WHITE", 0xFFFFFF}, {"BLACK", 0x000000}, {"YELLOW", 0xFFFF00},
    {"CYAN", 0x00FFFF}, {"MAGENTA", 0xFF00FF},
};
#define COLORS_SEQ_COUNT (int)(sizeof(COLORS_SEQ) / sizeof(COLORS_SEQ[0]))

void k85_run_color_test(void) {
    int idx = 0;
    int H = M5.Display.height();

    auto draw = [&]() {
        uint32_t val = COLORS_SEQ[idx].val;
        M5.Display.fillScreen(val);
        // Как в оригинале: тёмный текст только на светлых заливках (белый/жёлтый/циан)
        uint32_t txt_col = (val == 0xFFFFFF || val == 0xFFFF00 || val == 0x00FFFF) ? 0x000000 : 0xFFFFFF;
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(txt_col, val);
        M5.Display.setCursor(10, H / 2 - 8);
        M5.Display.print(COLORS_SEQ[idx].name);
    };

    draw();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            return;
        }
        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            idx = (idx + 1) % COLORS_SEQ_COUNT;
            draw();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
