#include "input.h"
#include "M5Unified.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void k85_input_update(void) { M5.update(); }

// K85CH1: a press made while the other button is held belongs to the A+B gesture, not to a single button
bool k85_btn_a_pressed(void) { return M5.BtnA.wasPressed() && !M5.BtnB.isPressed(); }
bool k85_btn_b_pressed(void) { return M5.BtnB.wasPressed() && !M5.BtnA.isPressed(); }
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

// K85PT1: pointer (tap) from the web page
static volatile int s_px = 0, s_py = 0;
static volatile int64_t s_pt = 0;
void k85_pointer_post(int x, int y) { s_px = x; s_py = y; s_pt = esp_timer_get_time(); }
bool k85_pointer_take(int *x, int *y) {
    int64_t t = s_pt;
    if (!t || esp_timer_get_time() - t > 600000) return false;
    *x = s_px; *y = s_py; s_pt = 0;
    return true;
}
// K85PT2: scroll steps from the web page
static volatile int s_sc = 0;
static volatile int64_t s_sct = 0;
void k85_scroll_post(int n) {
    if (n > 20) n = 20;
    if (n < -20) n = -20;
    int64_t t = esp_timer_get_time();
    if (t - s_sct > 600000) s_sc = 0;
    s_sc = s_sc + n;
    s_sct = t;
}
int k85_scroll_take(void) {
    int64_t t = s_sct;
    if (!t || esp_timer_get_time() - t > 600000) { s_sc = 0; return 0; }
    int v = s_sc;
    s_sc = 0;
    return v;
}

// K85PT3: age of a pending gesture
bool k85_pointer_age(int64_t *a) { int64_t t = s_pt; if (!t) return false; *a = esp_timer_get_time() - t; return true; }
bool k85_scroll_age(int64_t *a) { int64_t t = s_sct; if (!t || s_sc == 0) return false; *a = esp_timer_get_time() - t; return true; }
