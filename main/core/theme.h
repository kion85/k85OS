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

// Берёт индекс из g_config.theme_idx (с защитой от выхода за границы),
// охватывает и встроенные, и кастомные темы.
const k85_theme_t *k85_get_theme(void);

uint32_t k85_get_bg(void);
uint32_t k85_get_fg(void);
uint32_t k85_get_accent(void);

// ---------- Кастомные темы ----------
// Загружает темы из /littlefs/themes/*.thm (вызывать раз при старте).
// Формат файла (одна строка): name|BGHEX|FGHEX|ACCENTHEX|dark(0/1)|icon(bar/bolt)
// Например: MyTheme|001030|FFFFFF|00FFFF|0|bar
void k85_themes_load_custom(void);

// Общее число тем (встроенные + кастомные) — используй вместо K85_THEME_COUNT
// при переключении/переборе тем.
int k85_theme_count(void);

// Доступ по абсолютному индексу (0..k85_theme_count()-1)
const k85_theme_t *k85_get_theme_by_index(int idx);