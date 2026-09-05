#pragma once
// Циклически по 240 -> 160 -> 80 -> 240... Возвращает применённое значение.
int k85_cpu_freq_cycle(void);
// Применяет текущее g_config.cpu_freq_mhz (вызывать один раз при старте).
void k85_cpu_freq_apply_saved(void);