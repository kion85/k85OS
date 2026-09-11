#include "level.h"
#include "common.h"
#include "theme.h"
#include "input.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cmath>

void k85_run_level(void) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    int w = M5.Display.width();
    int h = M5.Display.height();
    int cx = w / 2;
    int cy = h / 2 - 4;
    int bezel_r = (w < h ? w : h) / 2 - 20;
    if (bezel_r < 20) bezel_r = 20;
    int bubble_r = bezel_r / 6;
    if (bubble_r < 4) bubble_r = 4;

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }

        float ax = 0, ay = 0, az = 0;
        M5.Imu.getAccel(&ax, &ay, &az);

        // Смещение пузыря пропорционально наклону (ax/ay ~ sin угла в единицах
        // g), масштабируем под размер рамки и ограничиваем радиусом, чтобы
        // пузырь не выезжал за край при сильном наклоне.
        float bx = ax * bezel_r * 1.6f;
        float by = ay * bezel_r * 1.6f;
        float dist = sqrtf(bx * bx + by * by);
        if (dist > (float)(bezel_r - bubble_r)) {
            float k = (float)(bezel_r - bubble_r) / dist;
            bx *= k;
            by *= k;
        }

        bool is_level = (fabsf(ax) < 0.03f && fabsf(ay) < 0.03f);

        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 4);
        M5.Display.print("Level");

        M5.Display.drawCircle(cx, cy, bezel_r, fg);
        M5.Display.drawFastHLine(cx - bezel_r, cy, bezel_r * 2, 0x444444);
        M5.Display.drawFastVLine(cx, cy - bezel_r, bezel_r * 2, 0x444444);

        uint32_t bubble_col = is_level ? 0x00FF00 : accent;
        M5.Display.fillCircle(cx + (int)bx, cy + (int)by, bubble_r, bubble_col);

        float ax_clamped = ax > 1.0f ? 1.0f : (ax < -1.0f ? -1.0f : ax);
        float ay_clamped = ay > 1.0f ? 1.0f : (ay < -1.0f ? -1.0f : ay);
        float pitch_deg = asinf(ax_clamped) * 180.0f / 3.14159f;
        float roll_deg  = asinf(ay_clamped) * 180.0f / 3.14159f;

        char line[32];
        snprintf(line, sizeof(line), "X: %+.1f  Y: %+.1f", pitch_deg, roll_deg);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, h - 12);
        M5.Display.print(line);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}