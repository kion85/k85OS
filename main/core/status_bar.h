#pragma once
#include <stdint.h>

// Биты sb_flags — каждый элемент статус-бара главного меню включается/выключается отдельно.
#define SB_BIT_BATTERY_PCT   (1u << 0)  // "72%"
#define SB_BIT_BATTERY_BOLT  (1u << 1)  // стиль иконки: молния вместо полоски
#define SB_BIT_TIME          (1u << 2)
#define SB_BIT_WIFI          (1u << 3)
#define SB_BIT_SOUND         (1u << 4)
#define SB_BIT_UPTIME        (1u << 5)
#define SB_BIT_RAM           (1u << 6)
#define SB_BIT_BLUETOOTH     (1u << 7)
#define SB_BIT_STEPS         (1u << 8)
#define SB_BIT_DATE          (1u << 9)
#define SB_BIT_TEMP          (1u << 10)

// Рисует статус-бар главного меню по текущим настройкам g_config.sb_flags / sb_bg_color.
// Вызывать вместо k85_draw_battery_icon() ТОЛЬКО в главном меню (menu.cpp).
void k85_status_bar_draw(void);
