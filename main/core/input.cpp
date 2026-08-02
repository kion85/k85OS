#include "input.h"
#include "M5Unified.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void k85_input_update(void) { M5.update(); }

bool k85_btn_a_pressed(void) { return M5.BtnA.wasPressed(); }
bool k85_btn_b_pressed(void) { return M5.BtnB.wasPressed(); }
bool k85_btn_a_is_down(void) { return M5.BtnA.isPressed(); }
bool k85_btn_b_is_down(void) { return M5.BtnB.isPressed(); }

bool k85_check_ab_hold(void) {
    return M5.BtnA.isPressed() && M5.BtnB.isPressed();
}

static uint32_t s_ab_hold_start = 0;

bool k85_ab_held(uint32_t ms) {
    if (k85_check_ab_hold()) {
        if (s_ab_hold_start == 0) s_ab_hold_start = now_ms();
        return (now_ms() - s_ab_hold_start) >= ms;
    }
    s_ab_hold_start = 0;
    return false;
}

void k85_wait_ab_release(void) {
    while (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
        M5.update();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_ab_hold_start = 0;
}
