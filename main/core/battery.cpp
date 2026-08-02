#include "battery.h"
#include "theme.h"
#include "M5Unified.h"

int k85_get_battery(void) {
    int32_t lvl = M5.Power.getBatteryLevel();  // 0-100, или -1
    if (lvl < 0) return -1;
    if (lvl > 100) lvl = 100;
    return (int)lvl;
}

bool k85_is_charging(void) {
    return M5.Power.isCharging();
}

void k85_draw_battery_icon(void) {
    int batt = k85_get_battery();
    if (batt < 0) return;

    int w = M5.Display.width();
    int x = w - 30;
    int y = 4;
    uint32_t fg = k85_get_fg();
    uint32_t bg = k85_get_bg();

    // корпус батареи
    M5.Display.drawRect(x, y, 24, 12, fg);
    M5.Display.fillRect(x + 24, y + 3, 2, 6, fg);

    // заполнение по проценту
    int fill_w = (batt * 20) / 100;
    uint32_t fill_color = (batt <= 20) ? 0xFF0000 : k85_get_accent();
    M5.Display.fillRect(x + 1, y + 1, 20, 10, bg);
    if (fill_w > 0) M5.Display.fillRect(x + 1, y + 1, fill_w, 10, fill_color);

    if (k85_is_charging()) {
        M5.Display.setTextColor(0xFFFF00, bg);
        M5.Display.setCursor(x - 12, y + 2);
        M5.Display.print("+");
    }
}
