#include "apps_menu.h"
#include "common.h"
#include "list_menu.h"
#include "wifi_scanner.h"
#include "tools/bt_scan.h"
#include "tools/calculator.h"
#include "../ble_hid/air_mouse_ble.h"
#include "router_menu.h"
#include "M5Unified.h"
#include <cstring>

static void draw_apps_icon(int cx, int cy, int r, const char *name, uint32_t col, uint32_t bg_col) {
    auto &d = M5.Display;
    if (!strcmp(name, "WiFi Scanner")) {
        d.fillRect(cx - r/2, cy + r/2 - 2, 3, 3, col);
        d.fillRect(cx - r/6, cy + r/4 - 2, 3, r/2, col);
        d.fillRect(cx + r/6, cy - 2, 3, r - 2, col);
        d.drawCircle(cx, cy, r, col);
    } else if (!strcmp(name, "BLE Scanner")) {
        d.drawLine(cx, cy - r, cx, cy + r, col);
        d.drawLine(cx, cy - r, cx + r/2, cy - r/2, col);
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy + r/2, cx + r/2, cy + r/2, col);
        d.drawLine(cx + r/2, cy + r/2, cx - r/2, cy - r/2, col);
        d.drawLine(cx - r/2, cy - r/2, cx, cy - r, col);
    } else if (!strcmp(name, "Calculator")) {
        d.drawRect(cx - r/2, cy - r, r, r * 2, col);
        for (int row = 0; row < 3; row++) {
            for (int c = 0; c < 2; c++) {
                d.fillRect(cx - r/2 + 3 + c * (r/2 - 2), cy - r/2 + row * (r/2), 4, 4, col);
            }
        }
    } else if (!strcmp(name, "Air Mouse BLE")) {
        d.fillTriangle(cx - r/2, cy - r, cx - r/2, cy + r/2, cx, cy + r/6, col);
        d.fillTriangle(cx - r/2, cy + r/2, cx, cy + r/6, cx - r/6, cy + r, col);
        d.drawLine(cx + r/4, cy - r, cx + r/4, cy, col);
        d.drawLine(cx + r/4, cy - r, cx + r/2, cy - r/2, col);
        d.drawLine(cx + r/2, cy - r/2, cx, cy - r/4, col);
    } else if (!strcmp(name, "Network / Router")) {
        d.drawRect(cx - r, cy - r/3, r * 2, r * 2 / 3, col);
        d.drawLine(cx - r/2, cy + r/3, cx - r/2, cy + r/3 + r/3, col);
        d.drawLine(cx + r/2, cy + r/3, cx + r/2, cy + r/3 + r/3, col);
        d.fillCircle(cx - r/2, cy, 2, col);
        d.fillCircle(cx + r/2, cy, 2, col);
    } else if (!strcmp(name, "Back")) {
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy, col);
        d.drawLine(cx - r/2, cy, cx + r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy, cx + r, cy, col);
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
}

void k85_run_apps_menu(void) {
    static const char *items[] = {"WiFi Scanner", "BLE Scanner", "Calculator", "Air Mouse BLE", "Network / Router", "Back"};
    while (true) {
        int idx = k85_run_list_menu("APPS", items, 6, nullptr, draw_apps_icon);
        if (idx < 0 || idx == 5) return;
        if (idx == 0) k85_run_wifi_scanner();
        else if (idx == 1) k85_run_bt_scan();
        else if (idx == 2) k85_run_calculator();
        else if (idx == 3) k85_run_air_mouse_ble();
        else if (idx == 4) k85_run_router_menu();
    }
}
