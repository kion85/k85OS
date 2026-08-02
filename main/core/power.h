#pragma once
#include <stdbool.h>
#include <stdint.h>

#define K85_BATTERY_MODE_COUNT 3
extern const char *k85_battery_modes[K85_BATTERY_MODE_COUNT];  // "Normal","Balanced","SuperEco"

#define K85_BRIGHTNESS_IDLE 15
#define K85_BRIGHTNESS_ECO  10

// Инициализация (сбрасывает таймер активности)
void k85_power_init(void);

// Вызывать каждый цикл главного loop: проверяет idle-таймаут и гасит экран.
// Возвращает true, если экран только что притушен (событие для UI - показать lock screen).
bool k85_power_tick(void);

// Сбросить таймер простоя + вернуть яркость (вызывать при любом действии юзера)
void k85_wake_screen(void);

bool k85_is_dimmed(void);

// idle-таймаут в мс для текущего battery_mode_idx
uint32_t k85_idle_timeout_ms(void);
