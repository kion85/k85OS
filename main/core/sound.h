#pragma once
#include <stdint.h>

// Применяет текущую громкость (g_config.sound_volume) к M5.Speaker
void k85_apply_sound_volume(void);

// Аналог play_tone(freq, dur_ms) из MicroPython
void k85_play_tone(uint32_t freq, uint32_t dur_ms);

void k85_speaker_stop(void);
