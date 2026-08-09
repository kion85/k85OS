#include "boot_screen.h"
#include "config.h"
#include "theme.h"
#include "boot_theme.h"
#include "input.h"
#include "../apps/k85os_menu.h"
#include "../apps/test_mode.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cmath>

#define K85_FW_VERSION "4.4"
#define K85_BOOT_DURATION_MS 4000
#define K85_BOOT_MENU_TIMEOUT_MS 3000

const char *k85_boot_style_names[K85_BOOT_STYLE_COUNT] = {
    "Classic bar", "Spinner circle", "Static text",
};

static K85BootTheme s_boot_theme;

static int boot_draw_title(void) {
    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = s_boot_theme.bg;
    uint32_t accent = s_boot_theme.accent;

    M5.Display.fillScreen(bg);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x888888, bg);
    M5.Display.setCursor(4, 4);
    M5.Display.print("v" K85_FW_VERSION);

    M5.Display.setTextSize(3);
    M5.Display.setTextColor(accent, bg);
    const char *text = "k85OS";
    int approx_w = (int)strlen(text) * 18;
    int x = (W - approx_w) / 2;
    int y = H / 2 - 30;
    M5.Display.setCursor(x, y);
    M5.Display.print(text);

    return y;
}

static void boot_classic_bar(int title_y) {
    int W = M5.Display.width();
    uint32_t accent = s_boot_theme.accent;

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
    uint32_t bg = s_boot_theme.bg;
    uint32_t accent = s_boot_theme.accent;

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
    uint32_t bg = s_boot_theme.bg;

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x888888, bg);
    const char *msg = "Loading...";
    int x = (W - (int)strlen(msg) * 6) / 2;
    int y = H / 2 + 30;
    M5.Display.setCursor(x, y);
    M5.Display.print(msg);

    vTaskDelay(pdMS_TO_TICKS(K85_BOOT_DURATION_MS));
}

// ---------- GRUB-style boot menu ----------
enum BootChoice { BOOT_NORMAL = 0, BOOT_BIOS = 1, BOOT_TEST = 2 };

static void draw_boot_menu(int selected, int seconds_left) {
    uint32_t bg = s_boot_theme.bg;
    uint32_t fg = s_boot_theme.fg;
    uint32_t accent = s_boot_theme.accent;

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(10, 10);
    M5.Display.print("k85OS Boot Menu");

    static const char *items[] = {"k85OS (normal)", "k85os-menu (BIOS)", "Test Mode"};
    int y = 50;
    int w = M5.Display.width();
    for (int i = 0; i < 3; i++) {
        bool sel = (i == selected);
        if (sel) {
            M5.Display.fillRect(2, y - 2, w - 4, 13, accent);
        }
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(sel ? bg : fg, sel ? accent : bg);
        M5.Display.setCursor(10, y);
        M5.Display.print(sel ? "> " : "  ");
        M5.Display.print(items[i]);
        y += 16;
    }

    M5.Display.setTextColor(0x777777, bg);
    M5.Display.setCursor(10, y + 10);
    if (seconds_left > 0) {
        M5.Display.printf("Auto-boot in %ds  A=select B=confirm", seconds_left);
    } else {
        M5.Display.print("A=select B=confirm");
    }
}

// Показывает GRUB-подобное меню на K85_BOOT_MENU_TIMEOUT_MS.
// Если пользователь ничего не нажал — обычная загрузка. Если нажал A —
// таймер отменяется, дальше свободная навигация.
static BootChoice run_boot_menu(void) {
    int selected = 0;
    bool interacted = false;
    int64_t start_us = esp_timer_get_time();
    int last_seconds_shown = -1;

    while (true) {
        k85_input_update();

        int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
        int seconds_left = interacted ? 0 : (int)((K85_BOOT_MENU_TIMEOUT_MS - elapsed_ms + 999) / 1000);
        if (seconds_left < 0) seconds_left = 0;

        if (seconds_left != last_seconds_shown) {
            draw_boot_menu(selected, seconds_left);
            last_seconds_shown = seconds_left;
        }

        if (k85_btn_a_pressed()) {
            interacted = true;
            selected = (selected + 1) % 3;
            draw_boot_menu(selected, 0);
            last_seconds_shown = 0;
        }
        if (k85_btn_b_pressed()) {
            return (BootChoice)selected;
        }

        if (!interacted && elapsed_ms >= K85_BOOT_MENU_TIMEOUT_MS) {
            return BOOT_NORMAL;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_show_boot_screen(void) {
    s_boot_theme = k85_boot_theme_load();

    BootChoice choice = run_boot_menu();

    if (choice == BOOT_BIOS) {
        k85_run_bios_menu();
        return;
    }
    if (choice == BOOT_TEST) {
        k85_run_test_mode();
        return;
    }

    int title_y = boot_draw_title();

    int style = g_config.bootstyle_idx;
    if (style < 0 || style >= K85_BOOT_STYLE_COUNT) style = 0;

    switch (style) {
        case 0: boot_classic_bar(title_y); break;
        case 1: boot_spinner(title_y); break;
        default: boot_static_text(title_y); break;
    }
}