#pragma once
#include <stdbool.h>

// Возвращает процент заряда 0-100, либо -1 если недоступно
int k85_get_battery(void);

bool k85_is_charging(void);

// Рисует иконку батареи в правом верхнем углу (аналог draw_battery_icon())
void k85_draw_battery_icon(void);
