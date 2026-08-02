#include "sound.h"
#include "config.h"
#include "M5Unified.h"

void k85_apply_sound_volume(void) {
    M5.Speaker.setVolume(k85_get_sound_volume());
}

void k85_play_tone(uint32_t freq, uint32_t dur_ms) {
    M5.Speaker.setVolume(k85_get_sound_volume());
    M5.Speaker.tone((float)freq, dur_ms);
}

void k85_speaker_stop(void) {
    M5.Speaker.stop();
}
