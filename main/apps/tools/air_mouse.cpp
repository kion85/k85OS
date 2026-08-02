#include "air_mouse.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "common.h"
#include "sound.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void k85_run_air_mouse(void) {
    float ax0, ay0, az0;
    if (!M5.Imu.getAccel(&ax0, &ay0, &az0)) {
        k85_show_message("IMU not\navailable");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    int cursor_x = W / 2;
    int cursor_y = H / 2;

    auto draw_crosshair = [&](int x, int y, bool clicked) {
        M5.Display.fillScreen(bg);
        uint32_t col = clicked ? 0xFF0000 : 0x00FFFF;
        M5.Display.drawLine(x - 8, y, x + 8, y, col);
        M5.Display.drawLine(x, y - 8, x, y + 8, col);
        M5.Display.fillCircle(x, y, 3, col);
        M5.Display.drawCircle(x, y, 10, col);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.print("Air Mouse");
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, H - 12);
        M5.Display.print("tilt=move A=click A+B=exit");
        k85_draw_battery_icon();
    };

    draw_crosshair(cursor_x, cursor_y, false);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        float ax = 0, ay = 0, az = 0;
        M5.Imu.getAccel(&ax, &ay, &az);

        cursor_x -= (int)(ax * 6);
        cursor_y += (int)(ay * 6);
        if (cursor_x < 2) cursor_x = 2;
        if (cursor_x > W - 2) cursor_x = W - 2;
        if (cursor_y < 2) cursor_y = 2;
        if (cursor_y > H - 2) cursor_y = H - 2;

        bool clicked = false;
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            clicked = true;
            k85_play_tone(800, 60);
        }
        draw_crosshair(cursor_x, cursor_y, clicked);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
