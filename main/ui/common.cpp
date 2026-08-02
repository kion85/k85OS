#include "common.h"
#include "theme.h"
#include "battery.h"
#include "M5Unified.h"
#include <cstring>

void k85_show_message(const char *text) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();

    int newline_count = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') newline_count++;
    }

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(fg, bg);

    char buf[256];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int y = M5.Display.height() / 2 - 8 - (newline_count * 10);
    char *saveptr = nullptr;
    char *line = strtok_r(buf, "\n", &saveptr);
    while (line) {
        M5.Display.setCursor(10, y);
        M5.Display.print(line);
        y += 20;
        line = strtok_r(nullptr, "\n", &saveptr);
    }

    k85_draw_battery_icon();
}