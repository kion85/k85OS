#include "boot_screen.h"
#include "config.h"
#include "theme.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cmath>

// Если в проекте уже есть макрос/константа версии прошивки (например в device.h) -
// замени эту строку на неё вместо дублирования.
#define K85_FW_VERSION "4.2"

#define K85_BOOT_DURATION_MS 4000

const char *k85_boot_style_names[K85_BOOT_STYLE_COUNT] = {
    "Classic bar", "Spinner circle", "Static text",
};

// Рисует версию в углу и заголовок "k85OS" по центру, возвращает Y заголовка
// (нужен, чтобы полоска/спиннер/текст рисовались чуть ниже него).
static int boot_draw_title(void) {
    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);

    // Версия в верхнем левом углу
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x888888, bg);
    M5.Display.setCursor(4, 4);
    M5.Display.print("v" K85_FW_VERSION);

    // Заголовок по центру
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(accent, bg);
    const char *text = "k85OS";
    int approx_w = (int)strlen(text) * 18; // ~ 6px * textSize(3) на символ
    int x = (W - approx_w) / 2;
    int y = H / 2 - 30;
    M5.Display.setCursor(x, y);
    M5.Display.print(text);

    return y;
}

static void boot_classic_bar(int title_y) {
    int W = M5.Display.width();
    uint32_t accent = k85_get_accent();

    int bar_w = 140;
    int bar_h = 6;
    int bar_x = (W - bar_w) / 2;
    int bar_y = title_y + 50;

    int64_t duration_us = (int64_t)K85_BOOT_DURATION_MS * 1000;
    int64_t start = esp_timer_get_time();

    while (esp_timer_get_time() - start < duration_us) {
        int64_t elapsed = esp_timer_get_time() - start;
        float progress = (float)elapsed / (float)duration_us;
        int fill_w = (int)(bar_w * progress);
        if (fill_w < 0) fill_w = 0;
        if (fill_w > bar_w) fill_w = bar_w;

        M5.Display.drawRect(bar_x, bar_y, bar_w, bar_h, 0x555555);
        int fw = fill_w - 2;
        if (fw < 0) fw = 0;
        M5.Display.fillRect(bar_x + 1, bar_y + 1, fw, bar_h - 2, accent);

        vTaskDelay(pdMS_TO_TICKS(30));
    }
    M5.Display.fillRect(bar_x + 1, bar_y + 1, bar_w - 2, bar_h - 2, accent);
    vTaskDelay(pdMS_TO_TICKS(150));
}

static void boot_spinner(int title_y) {
    int W = M5.Display.width();
    uint32_t bg = k85_get_bg();
    uint32_t accent = k85_get_accent();

    int cx = W / 2;
    int cy = title_y + 55;
    int r = 16;

    int64_t duration_us = (int64_t)K85_BOOT_DURATION_MS * 1000;
    int64_t start = esp_timer_get_time();
    float angle = 0.0f;
    const int dots = 8;

    while (esp_timer_get_time() - start < duration_us) {
        M5.Display.fillCircle(cx, cy, r + 4, bg);
        for (int i = 0; i < dots; i++) {
            float a = angle + i * (2.0f * (float)M_PI / dots);
            int px = cx + (int)(r * cosf(a));
            int py = cy + (int)(r * sinf(a));
            bool is_head = (i == 0);
            M5.Display.fillCircle(px, py, is_head ? 3 : 2, is_head ? accent : 0x004444);
        }
        angle += 0.4f;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    vTaskDelay(pdMS_TO_TICKS(150));
}

static void boot_static_text(int title_y) {
    (void)title_y;
    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = k85_get_bg();

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x888888, bg);
    const char *msg = "Loading...";
    int x = (W - (int)strlen(msg) * 6) / 2;
    int y = H / 2 + 30;
    M5.Display.setCursor(x, y);
    M5.Display.print(msg);

    vTaskDelay(pdMS_TO_TICKS(K85_BOOT_DURATION_MS));
}

void k85_show_boot_screen(void) {
    int title_y = boot_draw_title();

    int style = g_config.bootstyle_idx;
    if (style < 0 || style >= K85_BOOT_STYLE_COUNT) style = 0;

    switch (style) {
        case 0: boot_classic_bar(title_y); break;
        case 1: boot_spinner(title_y); break;
        default: boot_static_text(title_y); break;
    }
}


