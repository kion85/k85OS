#pragma once
#include <cstdint>

// Аналог run_list_menu() из MicroPython. items/score_keys — параллельные массивы длины count.
// score_keys[i] может быть nullptr для пункта без рекорда (например "Back").
// Возвращает индекс выбранного пункта, либо -1 если пользователь вышел (A+B, либо выбрал "Back").
//
// icon_fn — необязательный колбэк отрисовки иконки конкретного пункта (для grid/list+icons
// режимов, см. g_config.menu_ui_style). Если nullptr (по умолчанию) - используется
// универсальная иконка "первая буква пункта в кружке". Передавайте свою функцию, если
// хотите нарисовать под каждый пункт настоящую иконку (как в главном меню) - см. пример
// в tools_menu.cpp (draw_tools_icon).
//
// Сигнатура колбэка: void(int cx, int cy, int r, const char *item_name, uint32_t fg_col, uint32_t bg_col)
typedef void (*K85IconDrawFn)(int, int, int, const char *, uint32_t, uint32_t);

int k85_run_list_menu(const char *title, const char *const items[], int count,
                       const char *const score_keys[], K85IconDrawFn icon_fn = nullptr);