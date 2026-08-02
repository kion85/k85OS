#pragma once
#include <stdbool.h>
#include <stdint.h>

// Вызывать раз за цикл (обёртка над M5.update())
void k85_input_update(void);

bool k85_btn_a_pressed(void);   // wasPressed()
bool k85_btn_b_pressed(void);
bool k85_btn_a_is_down(void);   // isPressed()
bool k85_btn_b_is_down(void);

// Аналог check_ab_hold() из MicroPython: обе кнопки зажаты прямо сейчас
bool k85_check_ab_hold(void);

// Аналог ab_held(ms): обе кнопки удерживаются непрерывно >= ms миллисекунд.
// Нужно вызывать каждый цикл, копит время сама.
bool k85_ab_held(uint32_t ms);

// Аналог wait_ab_release(): блокирует, пока обе кнопки не отпустят
void k85_wait_ab_release(void);
