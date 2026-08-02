#pragma once

// Кол-во стилей загрузочного экрана и их читаемые имена (индекс = g_config.bootstyle_idx)
#define K85_BOOT_STYLE_COUNT 3
extern const char *k85_boot_style_names[K85_BOOT_STYLE_COUNT];

// Показывает загрузочный экран (~4 сек) по стилю из g_config.bootstyle_idx.
// Вызывать после M5.begin() и k85_config_load(), до входа в главное меню.
void k85_show_boot_screen(void);

