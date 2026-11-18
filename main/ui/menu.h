#pragma once

void k85_menu_init(void);
void k85_menu_draw(void);
void k85_menu_next(void);          // аналог BtnA.wasPressed() в главном меню
void k85_menu_prev(void);          // hold-A - непрерывная прокрутка назад
void k85_menu_activate(void);
void k85_menu_tap(int x, int y);   // K85PT1 // аналог BtnB.wasPressed() — запускает run_action(selected)

// Desktop/OS режим (g_config.desktop_mode): вызывать вместо обычной A/B
// логики в app_main - сам обновляет курсор, ловит наведение/клик, рисует
// рабочий стол с иконками + таскбар снизу.
void k85_menu_desktop_tick(void);