#include "theme.h"
#include "config.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

const k85_theme_t k85_themes[K85_THEME_COUNT] = {
    {"Navy",        0x001030, 0xFFFFFF, 0x00FFFF, false, "bar"},
    {"Dark Mode",   0x000000, 0xCCCCCC, 0x00FF00, true,  "bolt"},
    {"Retro Green", 0x001100, 0x00FF00, 0xFFFF00, true,  "bolt"},
    {"Light",       0xFFFFFF, 0x000000, 0x0000FF, false, "bar"},
    {"Sunset",      0x220011, 0xFFCC88, 0xFF6600, true,  "bolt"},
};

#define K85_CUSTOM_THEME_MAX 8
#define K85_THEMES_DIR "/littlefs"

static k85_theme_t s_custom_themes[K85_CUSTOM_THEME_MAX];
static char s_custom_names[K85_CUSTOM_THEME_MAX][24];
static char s_custom_icons[K85_CUSTOM_THEME_MAX][8];
static int s_custom_count = 0;

static bool parse_theme_line(const char *line, k85_theme_t *out, char *name_buf, char *icon_buf) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", line);

    char *fields[6] = {0};
    int n = 0;
    char *tok = buf;
    fields[n++] = tok;
    while (n < 6) {
        char *bar = strchr(tok, '|');
        if (!bar) break;
        *bar = 0;
        tok = bar + 1;
        fields[n++] = tok;
    }
    if (n < 6) return false;

    snprintf(name_buf, 24, "%.23s", fields[0]);
    out->name = name_buf;
    out->bg = (uint32_t)strtoul(fields[1], nullptr, 16);
    out->fg = (uint32_t)strtoul(fields[2], nullptr, 16);
    out->accent = (uint32_t)strtoul(fields[3], nullptr, 16);
    out->dark = (fields[4][0] == '1');
    snprintf(icon_buf, 8, "%.7s", fields[5]);
    out->batt_icon = icon_buf;
    return true;
}

void k85_themes_load_custom(void) {
    s_custom_count = 0;

    struct stat st;
    if (stat(K85_THEMES_DIR, &st) != 0) {
        mkdir(K85_THEMES_DIR, 0755);
        return; // РїР°РїРєР° С‚РѕР»СЊРєРѕ С‡С‚Рѕ СЃРѕР·РґР°РЅР°, С„Р°Р№Р»РѕРІ С‚Р°Рј РµС‰С‘ РЅРµС‚
    }

    DIR *d = opendir(K85_THEMES_DIR);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr && s_custom_count < K85_CUSTOM_THEME_MAX) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        char full[192];
        snprintf(full, sizeof(full), "%s/%s", K85_THEMES_DIR, ent->d_name);

        FILE *f = fopen(full, "r");
        if (!f) continue;

        char line[160];
        if (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            char *cr = strchr(line, '\r'); if (cr) *cr = 0;

            k85_theme_t theme = {};
            if (parse_theme_line(line, &theme,
                                  s_custom_names[s_custom_count],
                                  s_custom_icons[s_custom_count])) {
                s_custom_themes[s_custom_count] = theme;
                s_custom_count++;
            }
        }
        fclose(f);
    }
    closedir(d);
}

int k85_theme_count(void) {
    return K85_THEME_COUNT + s_custom_count;
}

const k85_theme_t *k85_get_theme_by_index(int idx) {
    if (idx < 0) idx = 0;
    int total = k85_theme_count();
    if (idx >= total) idx = total - 1;

    if (idx < K85_THEME_COUNT) {
        return &k85_themes[idx];
    }
    return &s_custom_themes[idx - K85_THEME_COUNT];
}

const k85_theme_t *k85_get_theme(void) {
    return k85_get_theme_by_index(g_config.theme_idx);
}

uint32_t k85_get_bg(void)     { return k85_get_theme()->bg; }
uint32_t k85_get_fg(void)     { return k85_get_theme()->fg; }
uint32_t k85_get_accent(void) { return k85_get_theme()->accent; }

#pragma GCC diagnostic pop


