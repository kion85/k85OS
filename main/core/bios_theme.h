#pragma once
#include <stdint.h>

struct K85BiosTheme {
    uint32_t bg;
    uint32_t fg;
    uint32_t accent;
};

// Читает /littlefs/bios/theme.thm (тот же формат, что .thm для обычных тем:
// name|BGHEX|FGHEX|ACCENTHEX|dark|icon — dark/icon для BIOS не используются).
// Если файла нет или он битый — возвращает дефолт: чёрный/белый/циан.
K85BiosTheme k85_bios_theme_load(void);
#define K85_BIOS_THEME_ACTIVE_FILE "/littlefs/bios/theme.thm"


