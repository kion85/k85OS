#pragma once
#include <stddef.h>

// Циклически по 240 -> 160 -> 80 -> 240... Возвращает применённое значение.
// Если частота зафиксирована в BIOS - ничего не меняет и возвращает значение из BIOS.
int k85_cpu_freq_cycle(void);
// Применяет текущую частоту (значение BIOS, если задано, иначе g_config.cpu_freq_mhz).
// Вызывать один раз при старте.
void k85_cpu_freq_apply_saved(void);

// ---- Настройка только для BIOS ----
// 0 = частотой управляет ОС (g_config.cpu_freq_mhz), иначе 80/160/240 задано в BIOS и имеет приоритет.
int k85_cpu_freq_bios_override(void);
// Переключает BIOS-настройку OS -> 80 -> 160 -> 240 -> OS, сразу применяет, возвращает новое значение (0 = OS).
int k85_cpu_freq_bios_cycle(void);
// Текст для строки BIOS: "OS" или "160 MHz".
void k85_cpu_freq_bios_label(char *out, size_t out_size);
