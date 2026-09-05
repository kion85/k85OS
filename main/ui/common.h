#pragma once
#include <stdint.h>
// Аналог show_message() из MicroPython — многострочный текст (\n) по центру экрана
void k85_show_message(const char *text);

// Рисует простую векторную иконку под конкретное название пункта меню
// (сопоставление по строке). Если под имя нет отдельной иконки - рисует
// маленький закрашенный кружок как generic fallback.
void k85_draw_item_icon(int cx, int cy, int r, const char *name, uint32_t col);