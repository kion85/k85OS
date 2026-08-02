#include "theme.h"
#include "config.h"

const k85_theme_t k85_themes[K85_THEME_COUNT] = {
    {"Navy",        0x001030, 0xFFFFFF, 0x00FFFF, false, "bar"},
    {"Dark Mode",   0x000000, 0xCCCCCC, 0x00FF00, true,  "bolt"},
    {"Retro Green", 0x001100, 0x00FF00, 0xFFFF00, true,  "bolt"},
    {"Light",       0xFFFFFF, 0x000000, 0x0000FF, false, "bar"},
    {"Sunset",      0x220011, 0xFFCC88, 0xFF6600, true,  "bolt"},
};

const k85_theme_t *k85_get_theme(void) {
    int idx = g_config.theme_idx;
    if (idx < 0 || idx >= K85_THEME_COUNT) idx = 0;
    return &k85_themes[idx];
}

uint32_t k85_get_bg(void)     { return k85_get_theme()->bg; }
uint32_t k85_get_fg(void)     { return k85_get_theme()->fg; }
uint32_t k85_get_accent(void) { return k85_get_theme()->accent; }
