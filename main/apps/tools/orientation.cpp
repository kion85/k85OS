#include "orientation.h"
#include "common.h"
#include "theme.h"
#include "input.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cmath>
#include <cstring>

// Определяет, какая из 6 граней (по доминирующей оси акселерометра
// относительно гравитации) сейчас "смотрит вверх". Точное соответствие
// оси физической стороне корпуса зависит от того, как вы держите
// устройство - подержите ровно каждой стороной вверх по очереди и
// запомните, какая метка чему соответствует у вас в руках.
static const char *detect_face(float ax, float ay, float az) {
    float aax = fabsf(ax), aay = fabsf(ay), aaz = fabsf(az);
    if (aaz >= aax && aaz >= aay) {
        return az > 0 ? "Z+" : "Z-";
    } else if (aax >= aay) {
        return ax > 0 ? "X+" : "X-";
    } else {
        return ay > 0 ? "Y+" : "Y-";
    }
}

void k85_run_orientation(void) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int w = M5.Display.width();
    int h = M5.Display.height();

    char last_face[4] = "";

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }

        float ax = 0, ay = 0, az = 0;
        M5.Imu.getAccel(&ax, &ay, &az);
        const char *face = detect_face(ax, ay, az);

        if (strcmp(face, last_face) != 0) {
            snprintf(last_face, sizeof(last_face), "%s", face);
        }

        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 4);
        M5.Display.print("Orientation");

        // Крупная метка текущей грани по центру
        M5.Display.setTextSize(4);
        M5.Display.setTextColor(fg, bg);
        int approx_w = (int)strlen(face) * 24;
        M5.Display.setCursor((w - approx_w) / 2, h / 2 - 20);
        M5.Display.print(face);

        // Сырые значения для самокалибровки (какая метка = какая физическая сторона)
        char line[40];
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xAAAAAA, bg);
        snprintf(line, sizeof(line), "X:%+.2f Y:%+.2f Z:%+.2f", ax, ay, az);
        M5.Display.setCursor((w - (int)strlen(line) * 6) / 2, h / 2 + 30);
        M5.Display.print(line);

        M5.Display.setCursor(4, h - 12);
        M5.Display.print("A+B=back");

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
