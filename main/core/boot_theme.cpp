#include "boot_theme.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>

K85BootTheme k85_boot_theme_load(void) {
    K85BootTheme t = { 0x000000, 0xFFFFFF, 0x00FFFF }; // дефолт

    struct stat st;
    if (stat(K85_BOOT_THEME_DIR, &st) != 0) {
        mkdir(K85_BOOT_THEME_DIR, 0755);
        return t;
    }

    FILE *f = fopen(K85_BOOT_THEME_ACTIVE_FILE, "r");
    if (!f) return t;

    char line[160];
    if (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;

        char *tok = line;
        char *fields[6] = {0};
        int n = 0;
        fields[n++] = tok;
        while (n < 6) {
            char *bar = strchr(tok, '|');
            if (!bar) break;
            *bar = 0;
            tok = bar + 1;
            fields[n++] = tok;
        }
        if (n >= 4) {
            t.bg = (uint32_t)strtoul(fields[1], nullptr, 16);
            t.fg = (uint32_t)strtoul(fields[2], nullptr, 16);
            t.accent = (uint32_t)strtoul(fields[3], nullptr, 16);
        }
    }
    fclose(f);
    return t;
}
