#pragma once

// Кол-во стилей загрузочного экрана и их читаемые имена (индекс = g_config.bootstyle_idx)
#define K85_BOOT_STYLE_COUNT 3
extern const char *k85_boot_style_names[K85_BOOT_STYLE_COUNT];

// Стили самого загрузчика (BIOS-меню выбора Normal/BIOS/Test/Alt FW),
// индекс = g_config.boot_loader_style.
#define K85_BOOT_LOADER_STYLE_COUNT 4
extern const char *k85_boot_loader_style_names[K85_BOOT_LOADER_STYLE_COUNT];

// Показывает загрузочный экран (~4 сек) по стилю из g_config.bootstyle_idx.
// Вызывать после M5.begin() и k85_config_load(), до входа в главное меню.
void k85_show_boot_screen(void);

// Если причина последнего сброса - паника/watchdog/brownout, показывает
// экран "kernel panic" (A+B или 15с - продолжить). Иначе сразу выходит.
// Вызывать рано в app_main(), сразу после M5.begin() - не требует LittleFS/config.
void k85_check_panic_screen(void);

// Переключает g_config.boot_loader_style по кругу. Конфиг не сохраняет -
// вызывающий код (BIOS settings) должен сам вызвать k85_config_save().
void k85_boot_loader_style_cycle(void);

