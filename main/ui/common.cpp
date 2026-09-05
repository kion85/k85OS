#include "common.h"
#include "theme.h"
#include "battery.h"
#include "M5Unified.h"
#include <cstring>
#include <cmath>

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

// Векторные иконки по названию пункта меню - изначально жили только в
// главном меню (menu.cpp), вынесены сюда, чтобы Tools/Games/Settings тоже
// могли их использовать через общий стилизованный движок меню.
void k85_draw_item_icon(int cx, int cy, int r, const char *name, uint32_t col) {
    auto &d = M5.Display;

    // ---- Иконки главного меню (перенесены без изменений) ----
    if (!strcmp(name, "Low tone") || !strcmp(name, "High tone") || !strcmp(name, "Both tones")) {
        d.fillRect(cx - r, cy - r/2, r/2, r, col);
        d.fillTriangle(cx - r/2, cy - r, cx - r/2, cy + r, cx + r/4, cy, col);
        int waves = !strcmp(name, "Low tone") ? 1 : (!strcmp(name, "High tone") ? 2 : 3);
        for (int i = 0; i < waves; i++) {
            int off = r/2 + i * 3;
            d.drawLine(cx + r/3, cy - off/2, cx + r/3 + off/2, cy - off, col);
            d.drawLine(cx + r/3, cy + off/2, cx + r/3 + off/2, cy + off, col);
        }
    } else if (!strcmp(name, "Cube")) {
        int s = r;
        d.drawRect(cx - s, cy - s/2, s, s, col);
        d.drawRect(cx - s + s/3, cy - s/2 - s/3, s, s, col);
        d.drawLine(cx - s, cy - s/2, cx - s + s/3, cy - s/2 - s/3, col);
        d.drawLine(cx, cy - s/2, cx + s/3, cy - s/2 - s/3, col);
        d.drawLine(cx - s, cy + s/2, cx - s + s/3, cy + s/2 - s/3, col);
        d.drawLine(cx, cy + s/2, cx + s/3, cy + s/2 - s/3, col);
    } else if (!strcmp(name, "Colors") || !strcmp(name, "Color Test")) {
        d.fillCircle(cx - r/2, cy - r/3, r/3, 0xFF0000);
        d.fillCircle(cx + r/2, cy - r/3, r/3, 0x00FF00);
        d.fillCircle(cx, cy + r/3, r/3, 0x0000FF);
    } else if (!strcmp(name, "Clock")) {
        d.drawCircle(cx, cy, r, col);
        d.drawLine(cx, cy, cx, cy - r + 2, col);
        d.drawLine(cx, cy, cx + r/2, cy, col);
    } else if (!strcmp(name, "WiFi") || !strcmp(name, "WiFi Manager")) {
        d.fillRect(cx - r/2, cy + r/2 - 2, 3, 3, col);
        d.fillRect(cx - r/6, cy + r/4 - 2, 3, r/2, col);
        d.fillRect(cx + r/6, cy - 2, 3, r - 2, col);
    } else if (!strcmp(name, "Apps")) {
        int s = r/2 - 1;
        d.fillRect(cx - r/2, cy - r/2, s, s, col);
        d.fillRect(cx + 2, cy - r/2, s, s, col);
        d.fillRect(cx - r/2, cy + 2, s, s, col);
        d.fillRect(cx + 2, cy + 2, s, s, col);
    } else if (!strcmp(name, "Tools")) {
        d.fillCircle(cx - r/2, cy - r/2, r/3, col);
        d.drawLine(cx - r/2, cy - r/2, cx + r/2, cy + r/2, col);
        d.drawLine(cx - r/2 + 1, cy - r/2, cx + r/2 + 1, cy + r/2, col);
    } else if (!strcmp(name, "Games")) {
        d.fillRoundRect(cx - r, cy - r/2, r * 2, r, r/3, col);
        d.fillCircle(cx + r/2, cy, 2, 0x000000);
        d.fillCircle(cx + r/2 + 5, cy - 3, 2, 0x000000);
    } else if (!strcmp(name, "Settings")) {
        d.fillCircle(cx, cy, r/2, col);
        for (int a = 0; a < 360; a += 45) {
            float rad = a * 3.14159f / 180.0f;
            int x1 = cx + (int)(cosf(rad) * (r/2));
            int y1 = cy + (int)(sinf(rad) * (r/2));
            int x2 = cx + (int)(cosf(rad) * r);
            int y2 = cy + (int)(sinf(rad) * r);
            d.drawLine(x1, y1, x2, y2, col);
        }
    } else if (!strcmp(name, "System info")) {
        d.drawCircle(cx, cy, r, col);
        d.fillRect(cx - 1, cy - r/3, 2, r/3, col);
        d.fillRect(cx - 1, cy - r/2 - 2, 2, 2, col);
    } else if (!strcmp(name, "Logs")) {
        d.drawFastHLine(cx - r, cy - r/2, r * 2 - r/3, col);
        d.drawFastHLine(cx - r, cy, r * 2, col);
        d.drawFastHLine(cx - r, cy + r/2, r * 2 - r/2, col);
    } else if (!strcmp(name, "Notifications")) {
        d.fillCircle(cx, cy - 2, r/2, col);
        d.fillRect(cx - r/4, cy + r/3, r/2, 2, col);
        d.drawCircle(cx, cy - r - 1, 2, col);
    } else if (!strcmp(name, "Interpreter")) {
        d.drawLine(cx - r/2, cy - r/2, cx - r/3, cy - r/4, col);
        d.drawLine(cx - r/3, cy - r/4, cx - r/3, cy + r/4, col);
        d.drawLine(cx - r/3, cy + r/4, cx - r/2, cy + r/2, col);
        d.drawLine(cx + r/2, cy - r/2, cx + r/3, cy - r/4, col);
        d.drawLine(cx + r/3, cy - r/4, cx + r/3, cy + r/4, col);
        d.drawLine(cx + r/3, cy + r/4, cx + r/2, cy + r/2, col);

    // ---- Новые иконки для TOOLS ----
    } else if (!strcmp(name, "Bluetooth Scan") || !strcmp(name, "Air Mouse BLE")) {
        d.drawLine(cx, cy - r, cx, cy + r, col);
        d.drawLine(cx, cy - r, cx + r/2, cy - r/2, col);
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy + r/2, cx, cy + r, col);
        d.drawLine(cx, cy - r, cx - r/2, cy - r/2, col);
        d.drawLine(cx - r/2, cy - r/2, cx + r/2, cy + r/2, col);
        d.drawLine(cx + r/2, cy + r/2, cx, cy + r, col);
    } else if (!strcmp(name, "I2C Scanner")) {
        d.drawRect(cx - r, cy - r/2, r * 2, r, col);
        d.drawFastVLine(cx - r/3, cy - r/2, r, col);
        d.drawFastVLine(cx + r/3, cy - r/2, r, col);
    } else if (!strcmp(name, "GPIO Control")) {
        d.drawCircle(cx - r/2, cy, 3, col);
        d.drawCircle(cx + r/2, cy - r/2, 3, col);
        d.drawCircle(cx + r/2, cy + r/2, 3, col);
        d.drawLine(cx - r/2 + 3, cy, cx + r/2 - 3, cy - r/2, col);
        d.drawLine(cx - r/2 + 3, cy, cx + r/2 - 3, cy + r/2, col);
    } else if (!strcmp(name, "Files")) {
        d.fillRoundRect(cx - r, cy - r/2, r, r/3, 1, col);
        d.drawRoundRect(cx - r, cy - r/3, r * 2, r, 2, col);
    } else if (!strcmp(name, "Music Player") || !strcmp(name, "Melodies")) {
        d.fillCircle(cx - r/2, cy + r/2, r/3, col);
        d.fillCircle(cx + r/3, cy + r/3, r/3, col);
        d.drawFastVLine(cx - r/2 + r/3, cy - r, r + r/2, col);
        d.drawFastVLine(cx + r/3 + r/3, cy - r/2, r/2 + r/3, col);
        d.drawLine(cx - r/2 + r/3, cy - r, cx + r/3 + r/3, cy - r/2, col);
    } else if (!strcmp(name, "Mic Test")) {
        d.fillRoundRect(cx - r/3, cy - r, r * 2/3, r, r/3, col);
        d.drawCircle(cx, cy, r, col);
        d.drawFastVLine(cx, cy + r, r/2, col);
    } else if (!strcmp(name, "Air Mouse (screen)")) {
        d.drawRoundRect(cx - r/2, cy - r, r, r * 2, r/3, col);
        d.drawFastVLine(cx, cy - r, r/2, col);
    } else if (!strcmp(name, "WiFi Hotspot")) {
        d.fillCircle(cx, cy + r/2, 2, col);
        for (int rr = r/3; rr <= r; rr += r/3) {
            d.drawCircle(cx, cy + r/2, rr, col);
        }
    } else if (!strcmp(name, "Calculator")) {
        d.drawRoundRect(cx - r/2, cy - r, r, r * 2, 2, col);
        d.fillRect(cx - r/3, cy - r/2, r * 2/3, r/4, col);
        d.fillCircle(cx - r/4, cy + r/6, 1, col);
        d.fillCircle(cx, cy + r/6, 1, col);
        d.fillCircle(cx + r/4, cy + r/6, 1, col);
        d.fillCircle(cx - r/4, cy + r/2, 1, col);
        d.fillCircle(cx, cy + r/2, 1, col);
        d.fillCircle(cx + r/4, cy + r/2, 1, col);
    } else if (!strcmp(name, "Terminal") || !strcmp(name, "SSH Connect") || !strcmp(name, "Web Terminal")) {
        d.drawRoundRect(cx - r, cy - r/2, r * 2, r, 2, col);
        d.drawLine(cx - r/2, cy - r/6, cx - r/6, cy, col);
        d.drawLine(cx - r/6, cy, cx - r/2, cy + r/6, col);
        d.drawFastHLine(cx, cy + r/6, r/3, col);
    } else if (!strcmp(name, "IR Remote")) {
        d.fillRoundRect(cx - r/3, cy - r, r * 2/3, r * 2, r/4, col);
        d.drawLine(cx + r/3, cy - r/2, cx + r/3 + r/3, cy - r/2 - r/4, col);
        d.drawLine(cx + r/3, cy, cx + r/3 + r/2, cy, col);
    } else if (!strcmp(name, "Task Manager")) {
        d.drawRect(cx - r, cy - r, r * 2, r * 2, col);
        d.fillRect(cx - r + 2, cy - r + 2, r - 2, r/2, col);
        d.fillRect(cx - r + 2, cy, r/2, r - 2, col);
    } else if (!strcmp(name, "LoRa")) {
        d.fillCircle(cx, cy, 2, col);
        for (int rr = r/3; rr <= r; rr += r/3) {
            for (int a = -45; a <= 45; a += 90) {
                float rad = a * 3.14159f / 180.0f;
                d.drawLine(cx, cy, cx + (int)(sinf(rad) * rr), cy - (int)(cosf(rad) * rr), col);
            }
        }
    } else if (!strcmp(name, "Internet SDR") || !strcmp(name, "Web Radio")) {
        d.drawCircle(cx, cy, r, col);
        for (int i = -2; i <= 2; i++) {
            int x = cx + i * (r/3);
            int h = (i == 0) ? r/2 : r/3;
            d.drawFastVLine(x, cy - h/2, h, col);
        }
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
}