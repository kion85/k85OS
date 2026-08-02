// k85OS v4.1 — порт на ESP-IDF C++
// Слой 4: core/rtc_ntp + steps/step_counter + core/device + system/system_info + system/logs_screen

#include "M5Unified.h"
#include "core/config.h"
#include "core/theme.h"
#include "core/input.h"
#include "core/power.h"
#include "core/battery.h"
#include "core/sound.h"
#include "core/log.h"
#include "core/rtc_ntp.h"
#include "core/device.h"
#include "core/boot_screen.h"
#include "steps/step_counter.h"
#include "ui/menu.h"
#include "ui/lock_screen.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void) {
    auto m5cfg = M5.config();
    M5.begin(m5cfg);

    k85_log_init();
    k85_log("Boot start");

    if (!k85_fs_init()) {
        M5.Display.fillScreen(0x000000);
        M5.Display.setTextColor(0xFF0000, 0x000000);
        M5.Display.setCursor(4, 4);
        M5.Display.println("LittleFS FAIL");
        return;
    }
    k85_config_load();
    k85_log("Config loaded");

    k85_rtc_ntp_init();

    M5.Display.setRotation(g_config.rotation);

    k85_power_init();
    M5.Display.setBrightness(g_config.brightness_active);
    k85_apply_sound_volume();

    k85_show_boot_screen();

    k85_menu_init();
    k85_menu_draw();
    k85_lock_screen_loop();
    k85_menu_draw();

    while (true) {
        k85_input_update();
        k85_step_counter_update();

        if (k85_power_tick()) {
            k85_lock_screen_loop();
            k85_menu_draw();
            continue;
        }

        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            k85_menu_next();
        }

        if (k85_btn_b_pressed()) {
            k85_wake_screen();
            k85_menu_activate();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

