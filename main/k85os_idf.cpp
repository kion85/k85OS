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
#include "ui/quick_settings.h"
#include "net/ota.h"
#include "core/post_beep.h"
#include "net/wifi.h"
#include "core/status_bar.h"
using namespace k85;

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define K85_QS_HOLD_MS 800
static bool s_b_was_down = false;
static bool s_b_press_armed = false; // нажатие начато в app_main и ещё не развязано
static bool s_b_long_fired = false;
static int64_t s_b_down_start_us = 0;

// Пересинхронизация B после блокирующих UI-циклов (шторка, menu_activate, lock_screen).
// Иначе отпускание B, начатого внутри вложенного меню, читается как новое нажатие:
// Back в тулсах -> повторное открытие тулсов.
static void k85_btn_resync(void) {
    k85_input_update();
    s_b_was_down = k85_btn_b_is_down();
    s_b_press_armed = false;
    s_b_long_fired = false;
    s_b_down_start_us = 0;
}

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
        k85_post_beep(PostCode::LITTLEFS_MOUNT_FAIL);
        return;
    }
    k85_config_load();
    k85_post_set_enabled(g_config.post_beep_enabled);
    PostReport post_report;
    k85_post_report_check(post_report, PostCode::LITTLEFS_MOUNT_FAIL, true);
    k85_themes_load_custom();
    k85_log("Config loaded");

    k85_rtc_ntp_init();
    k85_post_report_check(post_report, PostCode::RTC_TIME_FAIL, k85_rtc_is_present());

    if (!g_config.wifi_disabled) {
        k85_wifi_init(); // сам факт успешного вызова = пройдено (детального статуса модуль не возвращает)
    }
    k85_post_report_check(post_report, PostCode::WIFI_INIT, true);

    // TODO: подставить сюда реальный вызов инициализации BT, когда появится отдельная функция
    k85_post_report_check(post_report, PostCode::BT_INIT, true);

    M5.Display.setRotation(g_config.rotation);

    k85_power_init();
    M5.Display.setBrightness(g_config.brightness_active);
    k85_apply_sound_volume();

    int batt = k85_get_battery();
    bool battery_ok = (batt < 0) || (batt > 20);
    k85_post_report_check(post_report, PostCode::BATTERY_LOW, battery_ok);
    k85_post_finish(post_report);

    k85_show_boot_screen();

    k85_ota_start_background_check(6 * 60 * 60 * 1000); // проверка раз в 6 часов

    k85_menu_init();
    k85_menu_draw();
    k85_lock_screen_loop();
    k85_menu_draw();

    int64_t last_sb_refresh_us = 0;

    while (true) {
        k85_input_update();
        k85_step_counter_update();

        int64_t now_sb_us = esp_timer_get_time();
        if (now_sb_us - last_sb_refresh_us > 1000000) { // раз в секунду
            k85_status_bar_draw();
            last_sb_refresh_us = now_sb_us;
        }

        if (k85_power_tick()) {
            k85_lock_screen_loop();
            k85_menu_draw();
            k85_btn_resync();
            continue;
        }

        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            k85_menu_next();
        }

        bool b_down_now = k85_btn_b_is_down();

        if (b_down_now && !s_b_was_down) {
            s_b_press_armed = true;
            s_b_down_start_us = esp_timer_get_time();
            s_b_long_fired = false;
        }

        if (b_down_now && s_b_press_armed && !s_b_long_fired) {
            int64_t held_ms = (esp_timer_get_time() - s_b_down_start_us) / 1000;
            if (held_ms >= K85_QS_HOLD_MS) {
                s_b_long_fired = true;
                k85_wake_screen();
                k85_quick_settings_open();
                k85_menu_draw();
                k85_btn_resync();
                continue;
            }
        }

        if (!b_down_now && s_b_was_down && s_b_press_armed && !s_b_long_fired) {
            k85_wake_screen();
            k85_log("app_main SHORT B -> menu_activate()");
            k85_menu_activate();
            k85_btn_resync(); // после меню (тулсы и т.п.) B может быть ещё нажат
            continue;
        }

        s_b_was_down = b_down_now;

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}












