#include "post_beep.h"
#include "sound.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace k85 {

static bool s_enabled = true;
static int s_saved_volume = -1;

void k85_post_set_enabled(bool enabled) { s_enabled = enabled; }

static void boost_post_volume(void) {
    if (s_saved_volume >= 0) return; // уже поднято
    s_saved_volume = g_config.sound_volume;
    if (g_config.sound_volume < 80) {
        g_config.sound_volume = 80;
        k85_apply_sound_volume();
    }
}

static void restore_post_volume(void) {
    if (s_saved_volume < 0) return;
    g_config.sound_volume = s_saved_volume;
    k85_apply_sound_volume();
    s_saved_volume = -1;
}

static void beep(uint32_t freq, uint32_t dur_ms) {
    k85_play_tone(freq, dur_ms);
    vTaskDelay(pdMS_TO_TICKS(dur_ms));
    k85_speaker_stop();
    vTaskDelay(pdMS_TO_TICKS(80));
}

void k85_post_beep(PostCode code) {
    if (!s_enabled) return;
    boost_post_volume();
    switch (code) {
        case PostCode::LITTLEFS_MOUNT_FAIL:
            beep(200, 500);
            break;
        case PostCode::RTC_TIME_FAIL:
            beep(1200, 100); beep(1200, 100);
            break;
        case PostCode::BATTERY_LOW:
            beep(700, 100); beep(700, 100); beep(700, 100);
            break;
        case PostCode::WIFI_INIT:
            beep(900, 80); beep(1100, 80);
            break;
        case PostCode::BT_INIT:
            beep(1100, 80); beep(900, 80);
            break;
    }
}

void k85_post_report_check(PostReport &report, PostCode code, bool ok) {
    if (!ok) {
        report.all_ok = false;
        k85_post_beep(code);
    }
}

void k85_post_finish(PostReport &report) {
    if (!s_enabled) return;
    if (report.all_ok) {
        boost_post_volume();
        beep(1800, 120);
        beep(2200, 120);
    }
    restore_post_volume();
}

} // namespace k85


