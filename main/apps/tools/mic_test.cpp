#include "mic_test.h"
#include "theme.h"
#include "power.h"
#include "input.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cmath>
#include <cstdio>

void k85_run_mic_test(void) {
    M5.Mic.begin();

    int W = M5.Display.width();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(4, 4);
    M5.Display.print("Mic test  A+B=exit");

    int bar_x = 20, bar_y = 30, bar_w = W - 40, bar_h = 20;
    int16_t samples[256];

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            M5.Mic.end();
            return;
        }

        int level = 0;
        if (M5.Mic.record(samples, 256, 16000)) {
            double total = 0;
            for (int i = 0; i < 256; i++) total += (double)samples[i] * (double)samples[i];
            double rms = sqrt(total / 256.0);
            level = (int)(rms / 327.67);
            if (level > 100) level = 100;
        }

        M5.Display.fillRect(bar_x, bar_y, bar_w, bar_h, 0x222222);
        int fill = bar_w * level / 100;
        uint32_t col = level < 60 ? 0x00FF00 : (level < 85 ? 0xFFFF00 : 0xFF0000);
        M5.Display.fillRect(bar_x, bar_y, fill, bar_h, col);
        M5.Display.drawRect(bar_x, bar_y, bar_w, bar_h, fg);
        M5.Display.fillRect(4, bar_y + 30, W - 8, 12, bg);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, bar_y + 30);
        M5.Display.printf("Level: %d%%", level);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

