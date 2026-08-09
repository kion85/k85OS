#pragma once
#include <stdint.h>

struct K85BootTheme {
    uint32_t bg;
    uint32_t fg;
    uint32_t accent;
};

#define K85_BOOT_THEME_DIR "/littlefs/grub"
#define K85_BOOT_THEME_ACTIVE_FILE "/littlefs/grub/theme.thm"

// Читает активную тему загрузочного экрана. Если файла нет — дефолт (чёрный/белый/циан).
K85BootTheme k85_boot_theme_load(void);
