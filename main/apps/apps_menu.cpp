#include "apps_menu.h"
#include "common.h"
#include "list_menu.h"
#include "wifi_scanner.h"
#include "tools/bt_scan.h"
#include "tools/calculator.h"
#include "../ble_hid/air_mouse_ble.h"

void k85_run_apps_menu(void) {
    static const char *items[] = {"WiFi Scanner", "BLE Scanner", "Calculator", "Air Mouse BLE", "Back"};
    while (true) {
        int idx = k85_run_list_menu("APPS", items, 5, nullptr);
        if (idx < 0 || idx == 4) return;
        if (idx == 0) k85_run_wifi_scanner();
        else if (idx == 1) k85_run_bt_scan();
        else if (idx == 2) k85_run_calculator();
        else if (idx == 3) k85_run_air_mouse_ble();
    }
}
