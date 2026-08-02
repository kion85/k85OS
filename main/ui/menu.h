#pragma once

void k85_menu_init(void);
void k85_menu_draw(void);
void k85_menu_next(void);     // аналог BtnA.wasPressed() в главном меню
void k85_menu_activate(void); // аналог BtnB.wasPressed() — запускает run_action(selected)