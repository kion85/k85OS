#include "sound.h"
#include "config.h"
#include "M5Unified.h"

void k85_apply_sound_volume(void) {
    if (g_config.sound_muted) {
        M5.Speaker.setVolume(0);
        return;
    }
    int percent = k85_get_sound_volume();
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint8_t vol255 = (uint8_t)((percent * 255) / 100);
    M5.Speaker.setVolume(vol255);
}

void k85_play_tone(uint32_t freq, uint32_t dur_ms) {
    if (g_config.sound_muted) return;
    M5.Speaker.tone((float)freq, dur_ms);
}

void k85_speaker_stop(void) {
    M5.Speaker.stop();
}
