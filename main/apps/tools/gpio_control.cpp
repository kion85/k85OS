#include "gpio_control.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "input.h"
#include "common.h"

#include "M5Unified.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

struct GpioPin { const char *name; int num; };

// Тот же список пинов, что и GPIO_PINS в оригинале
static const GpioPin GPIO_PINS[] = {
    {"GPIO2", 2}, {"GPIO5", 5}, {"GPIO12", 12}, {"GPIO13", 13},
    {"GPIO14", 14}, {"GPIO15", 15}, {"GPIO16", 16}, {"GPIO17", 17},
    {"GPIO18", 18}, {"GPIO19", 19}, {"GPIO21", 21}, {"GPIO22", 22},
    {"GPIO23", 23}, {"GPIO25", 25}, {"GPIO26", 26}, {"GPIO27", 27},
    {"GPIO32", 32}, {"GPIO33", 33}, {"GPIO34", 34}, {"GPIO35", 35},
    {"GPIO36", 36}, {"GPIO39", 39},
};
#define K85_GPIO_PIN_COUNT (int)(sizeof(GPIO_PINS) / sizeof(GPIO_PINS[0]))

void k85_run_gpio_test(void) {
    int sel = 0;
    bool mode_output = true;
    int gpio_val = 0;

    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    auto draw = [&]() {
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.print("GPIO Test");

        M5.Display.setTextSize(2);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(10, 20);
        M5.Display.print(GPIO_PINS[sel].name);

        M5.Display.setTextSize(1);
        M5.Display.setCursor(10, 42);
        M5.Display.printf("Mode: %s", mode_output ? "output" : "input");

        if (mode_output) {
            M5.Display.setCursor(10, 56);
            M5.Display.printf("Value: %s", gpio_val ? "HIGH" : "LOW");
        } else {
            int level = gpio_get_level((gpio_num_t)GPIO_PINS[sel].num);
            M5.Display.setCursor(10, 56);
            M5.Display.printf("Read: %d", level);
        }

        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, H - 28);
        M5.Display.print("A=next  B=toggle/read");
        M5.Display.setCursor(4, H - 14);
        M5.Display.print("2xA(<400ms)=mode  A+B=exit");
        k85_draw_battery_icon();
    };

    auto apply_mode = [&]() {
        int num = GPIO_PINS[sel].num;
        gpio_reset_pin((gpio_num_t)num);
        if (mode_output) {
            gpio_set_direction((gpio_num_t)num, GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)num, gpio_val);
        } else {
            gpio_set_direction((gpio_num_t)num, GPIO_MODE_INPUT);
        }
    };

    apply_mode();
    draw();

    // Оригинал детектил "зажатие A после клика" через isPressed() сразу после wasPressed().
    // У нас нет обёртки над level-check кнопки (k85_input.h её не даёт), поэтому переключение
    // режима сделано через двойной клик A (<400мс между нажатиями) — тот же принцип 2xA,
    // что уже используется в keyboard/text_input.cpp этого проекта.
    int64_t last_a_ms = 0;
    const int64_t double_ms = 400;

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            gpio_reset_pin((gpio_num_t)GPIO_PINS[sel].num);
            gpio_set_direction((gpio_num_t)GPIO_PINS[sel].num, GPIO_MODE_INPUT);
            return;
        }
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            int64_t now = (int64_t)(esp_timer_get_time() / 1000);
            if (now - last_a_ms < double_ms) {
                mode_output = !mode_output;
                apply_mode();
            } else {
                sel = (sel + 1) % K85_GPIO_PIN_COUNT;
                apply_mode();
            }
            last_a_ms = now;
            draw();
        }
        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            if (mode_output) {
                gpio_val = gpio_val ? 0 : 1;
                gpio_set_level((gpio_num_t)GPIO_PINS[sel].num, gpio_val);
            }
            draw();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}


