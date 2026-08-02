#pragma once

// Аналог run_list_menu() из MicroPython. items/score_keys — параллельные массивы длины count.
// score_keys[i] может быть nullptr для пункта без рекорда (например "Back").
// Возвращает индекс выбранного пункта, либо -1 если пользователь вышел (A+B, либо выбрал "Back").
int k85_run_list_menu(const char *title, const char *const items[], int count,
                       const char *const score_keys[]);
