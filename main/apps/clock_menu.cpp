#include "clock_menu.h"
#include "common.h"
#include "theme.h"
#include "battery.h"
#include "input.h"
#include "sound.h"
#include "config.h"
#include "rtc_ntp.h"
#include "wifi.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <cstdio>
#include <cstring>
#include <ctime>

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Часы ----------
static void run_clock_face(void) {
    if (!k85_is_ntp_synced()) {
        if (k85_wifi_is_connected()) {
            k85_ntp_sync_now();
        }
    }

    uint32_t bg = k85_get_bg();
    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(k85_get_accent(), bg);
    M5.Display.setCursor(10, 6);
    M5.Display.printf("Clock [%s]", k85_is_ntp_synced() ? "NTP" : "NO NTP");

    int scr_w = M5.Display.width();
    int scr_h = M5.Display.height();
    uint32_t elapsed_ms = 500;

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }

        if (elapsed_ms >= 500) {
            elapsed_ms = 0;
            const char *t = k85_get_time_str();
            M5.Display.setTextSize(3);
            M5.Display.setTextColor(k85_get_fg(), k85_get_bg());
            int approx_w = (int)strlen(t) * 18;
            M5.Display.setCursor((scr_w - approx_w) / 2, scr_h / 2 - 16);
            M5.Display.print(t);

            if (k85_is_ntp_synced()) {
                const char *d = k85_get_date_str();
                M5.Display.setTextSize(1);
                M5.Display.setTextColor(0x888888, k85_get_bg());
                M5.Display.setCursor((scr_w - (int)strlen(d) * 6) / 2, scr_h / 2 + 8);
                M5.Display.print(d);
            }
            k85_draw_battery_icon();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed_ms += 50;
    }
}

// ---------- Громкость сигнала: временный подъём на время будильника/таймера ----------
static int s_saved_volume = -1;

static void boost_alarm_volume(void) {
    static const int percents[4] = {40, 60, 80, 100};
    int i = g_config.alarm_volume_idx;
    if (i < 0 || i > 3) i = 2;
    s_saved_volume = g_config.sound_volume;
    g_config.sound_volume = percents[i];
    k85_apply_sound_volume();
}

static void restore_volume(void) {
    if (s_saved_volume < 0) return;
    g_config.sound_volume = s_saved_volume;
    k85_apply_sound_volume();
    s_saved_volume = -1;
}

// ---------- Общий сигнал (низкий-высокий, пауза, повтор) ----------
static bool play_alarm_pattern_step(void) {
    k85_play_tone(300, 250);
    for (int i = 0; i < 5; i++) {
        k85_input_update();
        if (k85_ab_held(300)) { k85_wait_ab_release(); k85_speaker_stop(); return true; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    k85_speaker_stop();
    k85_play_tone(1200, 250);
    for (int i = 0; i < 5; i++) {
        k85_input_update();
        if (k85_ab_held(300)) { k85_wait_ab_release(); k85_speaker_stop(); return true; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    k85_speaker_stop();
    for (int i = 0; i < 6; i++) {
        k85_input_update();
        if (k85_ab_held(300)) { k85_wait_ab_release(); return true; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

// ---------- Ввод числа (0..max) через A=+1 B=подтвердить ----------
static int input_number_field(const char *label, int initial, int max_val) {
    int val = initial;
    uint32_t bg = k85_get_bg();
    while (true) {
        k85_input_update();
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(k85_get_fg(), bg);
        M5.Display.setCursor(10, 40);
        M5.Display.printf("%s", label);
        M5.Display.setCursor(10, 70);
        M5.Display.printf("%02d", val);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(10, 110);
        M5.Display.print("A=+1  B=OK  A+B=cancel");

        if (k85_ab_held(500)) { k85_wait_ab_release(); return -1; }
        if (k85_btn_a_pressed()) { val = (val + 1) % (max_val + 1); }
        if (k85_btn_b_pressed()) { return val; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Таймер ----------
static void run_timer(void) {
    int h = input_number_field("Timer: Hours", 0, 23);
    if (h < 0) return;
    int m = input_number_field("Timer: Minutes", 0, 59);
    if (m < 0) return;
    int s = input_number_field("Timer: Seconds", 0, 59);
    if (s < 0) return;

    int64_t total_sec = (int64_t)h * 3600 + m * 60 + s;
    if (total_sec <= 0) {
        k85_show_message("Timer: 0s, cancelled\nA+B=back");
        wait_ab_exit();
        return;
    }

    int64_t start_us = esp_timer_get_time();
    uint32_t bg = k85_get_bg();

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }

        int64_t elapsed_sec = (esp_timer_get_time() - start_us) / 1000000;
        int64_t remain = total_sec - elapsed_sec;
        if (remain <= 0) break;

        int rh = (int)(remain / 3600);
        int rm = (int)((remain % 3600) / 60);
        int rs = (int)(remain % 60);

        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(3);
        M5.Display.setTextColor(k85_get_fg(), bg);
        M5.Display.setCursor(10, 50);
        M5.Display.printf("%02d:%02d:%02d", rh, rm, rs);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(10, 100);
        M5.Display.print("A+B=cancel");

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    boost_alarm_volume();
    k85_show_message("TIMER DONE!\nA+B=stop");
    while (true) {
        if (play_alarm_pattern_step()) break;
    }
    restore_volume();
    k85_show_message("Timer finished\nB=back");
    while (true) {
        k85_input_update();
        if (k85_btn_b_pressed()) return;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Будильник ----------
static void run_alarm(void) {
    int h = input_number_field("Alarm: Hours", 7, 23);
    if (h < 0) return;
    int m = input_number_field("Alarm: Minutes", 0, 59);
    if (m < 0) return;

    char msg[64];
    snprintf(msg, sizeof(msg), "Alarm set: %02d:%02d\nWaiting...\nA+B=cancel", h, m);
    k85_show_message(msg);

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }

        if (k85_is_ntp_synced()) {
            time_t now;
            time(&now);
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            if (timeinfo.tm_hour == h && timeinfo.tm_min == m) break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    boost_alarm_volume();
    k85_show_message("ALARM!\nA+B=stop");
    while (true) {
        if (play_alarm_pattern_step()) break;
    }
    restore_volume();
    k85_show_message("Alarm stopped\nB=back");
    while (true) {
        k85_input_update();
        if (k85_btn_b_pressed()) return;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Настройки времени ----------
static void run_time_settings(void) {
    static const char *items[] = {"Set time", "Set date", "Timezone (UTC)", "Back"};
    int selected = 0;
    uint32_t bg = k85_get_bg();

    while (true) {
        k85_input_update();
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(k85_get_fg(), bg);
        M5.Display.setCursor(6, 6);
        M5.Display.print("Time settings");

        char val[24];
        for (int i = 0; i < 4; i++) {
            bool sel = (i == selected);
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(sel ? k85_get_accent() : k85_get_fg(), bg);
            M5.Display.setCursor(6, 36 + i * 16);
            M5.Display.print(sel ? "> " : "  ");
            M5.Display.print(items[i]);
            if (i == 2) {
                snprintf(val, sizeof(val), "UTC%+d", g_config.utc_offset);
                M5.Display.setCursor(140, 36 + i * 16);
                M5.Display.print(val);
            }
        }
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(6, 110);
        M5.Display.print("A=next B=select A+B=back");

        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed()) { selected = (selected + 1) % 4; }
        if (k85_btn_b_pressed()) {
            if (selected == 3) return;
            if (selected == 0) {
                int h = input_number_field("Set: Hours", 0, 23);
                if (h < 0) continue;
                int m = input_number_field("Set: Minutes", 0, 59);
                if (m < 0) continue;
                k85_rtc_set_manual_time(h, m, 0);
                k85_show_message("Time set!\nA+B=back");
                wait_ab_exit();
            } else if (selected == 1) {
                int day = input_number_field("Set: Day", 1, 31);
                if (day < 1) continue;
                int month = input_number_field("Set: Month", 1, 12);
                if (month < 1) continue;
                int year_offset = input_number_field("Set: Year (2000+)", 25, 50);
                if (year_offset < 0) continue;
                k85_rtc_set_manual_date(day, month, 2000 + year_offset);
                k85_show_message("Date set!\nA+B=back");
                wait_ab_exit();
            } else if (selected == 2) {
                int off = g_config.utc_offset;
                int shifted = input_number_field("UTC offset (0=-12..24=+12)", off + 12, 24);
                if (shifted < 0) continue;
                g_config.utc_offset = shifted - 12;
                k85_rtc_apply_tz(g_config.utc_offset);
                k85_config_save();
                k85_show_message("Timezone updated!\nA+B=back");
                wait_ab_exit();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Главное суб-меню часов ----------
void k85_run_clock_menu(void) {
    static const char *items[] = {"Clock", "Timer", "Alarm", "Time settings", "Back"};
    int selected = 0;
    uint32_t bg = k85_get_bg();

    while (true) {
        k85_input_update();
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(k85_get_fg(), bg);
        M5.Display.setCursor(6, 6);
        M5.Display.print("Clock menu");

        for (int i = 0; i < 5; i++) {
            bool sel = (i == selected);
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(sel ? k85_get_accent() : k85_get_fg(), bg);
            M5.Display.setCursor(6, 36 + i * 16);
            M5.Display.print(sel ? "> " : "  ");
            M5.Display.print(items[i]);
        }
        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(6, 130);
        M5.Display.print("A=next B=select A+B=exit");

        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed()) { selected = (selected + 1) % 5; }
        if (k85_btn_b_pressed()) {
            if (selected == 4) return;
            if (selected == 0) run_clock_face();
            else if (selected == 1) run_timer();
            else if (selected == 2) run_alarm();
            else if (selected == 3) run_time_settings();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}