#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name;
    uint32_t bg;
    uint32_t fg;
    uint32_t accent;
    bool dark;
    const char *batt_icon;  // "bar" | "bolt"
} k85_theme_t;

#define K85_THEME_COUNT 5

extern const k85_theme_t k85_themes[K85_THEME_COUNT];

// Берёт индекс из g_config.theme_idx (с защитой от выхода за границы)
const k85_theme_t *k85_get_theme(void);

uint32_t k85_get_bg(void);
uint32_t k85_get_fg(void);
uint32_t k85_get_accent(void);
