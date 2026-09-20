#include "boot_screen.h"
#include "config.h"
#include "theme.h"
#include "boot_theme.h"
#include "input.h"
#include "../apps/k85os_menu.h"
#include "../apps/test_mode.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cmath>

#define K85_FW_VERSION "7.3"
#define K85_BOOT_DURATION_MS 4000
#define K85_BOOT_MENU_TIMEOUT_MS 3000

const char *k85_boot_style_names[K85_BOOT_STYLE_COUNT] = {
    "Classic bar", "Spinner circle", "Static text",
};

const char *k85_boot_loader_style_names[K85_BOOT_LOADER_STYLE_COUNT] = {
    "k85OS Boot Menu", "k85OS BIOS Boot", "GRUB-style", "rEFInd-style",
};

void k85_boot_loader_style_cycle(void) {
    g_config.boot_loader_style = (g_config.boot_loader_style + 1) % K85_BOOT_LOADER_STYLE_COUNT;
}

static K85BootTheme s_boot_theme;
static bool try_draw_custom_logo(const K85BootTheme &theme);

static int boot_draw_title(void) {
    int W = M5.Display.width();
    int H = M5.Display.height();
    uint32_t bg = s_boot_theme.bg;
    uint32_t accent = s_boot_theme.accent;

    if (try_draw_custom_logo(s_boot_theme)) return H / 2 - 30;

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

// ---------- Р›РѕРіРѕС‚РёРї: С‡РёР±Рё-РєР°РїРёР±Р°СЂР° ----------
static void draw_capybara_logo(int cx, int cy) {
    uint32_t body_col   = 0x8B6F47;
    uint32_t muzzle_col = 0xE8D5B0;
    uint32_t dark_col   = 0x3A2E20;
    uint32_t orange_col = 0xFFA500;
    uint32_t orange_seg = 0xFFF3D6;

    M5.Display.fillRoundRect(cx - 32, cy + 6, 64, 30, 14, body_col);
    M5.Display.fillCircle(cx, cy - 6, 26, body_col);
    M5.Display.fillCircle(cx - 16, cy - 26, 7, body_col);
    M5.Display.fillCircle(cx + 16, cy - 26, 7, body_col);
    M5.Display.fillCircle(cx - 16, cy - 26, 4, dark_col);
    M5.Display.fillCircle(cx + 16, cy - 26, 4, dark_col);
    M5.Display.fillRoundRect(cx - 16, cy - 2, 32, 18, 8, muzzle_col);
    M5.Display.fillRoundRect(cx - 12, cy - 8, 6, 3, 1, dark_col);
    M5.Display.fillRoundRect(cx + 6,  cy - 8, 6, 3, 1, dark_col);
    M5.Display.fillTriangle(cx - 4, cy + 2, cx + 4, cy + 2, cx, cy + 7, dark_col);

    int ox = cx, oy = cy - 34;
    M5.Display.fillCircle(ox, oy, 10, orange_col);
    for (int i = -2; i <= 2; i++) {
        M5.Display.drawLine(ox, oy, ox + i * 3, oy - 9, orange_seg);
    }
    M5.Display.drawCircle(ox, oy, 10, orange_seg);
}

#define K85_LOGO_DURATION_MS 1000
#define K85_CUSTOM_LOGO_PATH "/littlefs/boot_logo.jpg"

// Пользователь может залить свою картинку через веб-файловый менеджер
// (Tools -> WiFi Hotspot -> веб-интерфейс), назвав файл boot_logo.jpg
// в корне LittleFS — тогда она используется вместо капибары.
static bool try_draw_custom_logo(const K85BootTheme &theme) {
    FILE *f = fopen(K85_CUSTOM_LOGO_PATH, "rb");
    if (!f) return false;
    fclose(f);

    int W = M5.Display.width();
    int H = M5.Display.height();
    M5.Display.fillScreen(theme.bg);
    bool ok = M5.Display.drawJpgFile(K85_CUSTOM_LOGO_PATH, 0, 0, W, H, 0, 0, lgfx::jpeg_div::JPEG_DIV_NONE);
    return ok;
}

static void show_pre_boot_logo(const K85BootTheme &theme) {
    int W = M5.Display.width();
    int H = M5.Display.height();

    M5.Display.fillScreen(theme.bg);
    if (try_draw_custom_logo(theme)) {
        vTaskDelay(pdMS_TO_TICKS(K85_LOGO_DURATION_MS));
        return;
    }
    draw_capybara_logo(W / 2, H / 2 - 10);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(theme.accent, theme.bg);
    const char *text = "k85OS";
    int approx_w = (int)strlen(text) * 6;
    M5.Display.setCursor((W - approx_w) / 2, H / 2 + 44);
    M5.Display.print(text);

    vTaskDelay(pdMS_TO_TICKS(K85_LOGO_DURATION_MS));
}

// ---------- GRUB-style boot menu ----------
enum BootChoice { BOOT_NORMAL = 0, BOOT_BIOS = 1, BOOT_TEST = 2, BOOT_ALT_FW = 3 };
#define K85_BOOT_MENU_ITEM_COUNT 4

static void draw_boot_menu(int selected, int seconds_left) {
    uint32_t bg = s_boot_theme.bg;
    uint32_t fg = s_boot_theme.fg;
    uint32_t accent = s_boot_theme.accent;

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(10, 10);
    M5.Display.print("k85OS Boot Menu");

    static const char *items[K85_BOOT_MENU_ITEM_COUNT] = {
        "k85OS (normal)", "k85os-menu (BIOS)", "Test Mode", "Alt Firmware"
    };
    int y = 42;
    int w = M5.Display.width();
    for (int i = 0; i < K85_BOOT_MENU_ITEM_COUNT; i++) {
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

// ---------- k85OS BIOS Boot: строгий текстовый стиль, диски как FlashN ----------
static const char *k85_bootloader_items_bios[K85_BOOT_MENU_ITEM_COUNT] = {
    "Flash1 (Normal)", "Flash2 (BIOS)", "Flash3 (Test)", "Flash4 (Alt FW)"
};

static void draw_boot_menu_bios(int selected, int seconds_left) {
    uint32_t bg = 0x000000;
    uint32_t fg = 0xC0C0C0;
    uint32_t accent = 0xFFFFFF;

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(4, 4);
    M5.Display.print("k85OS BIOS Boot");
    M5.Display.setCursor(4, 14);
    M5.Display.print("Select boot device:");

    int y = 30;
    for (int i = 0; i < K85_BOOT_MENU_ITEM_COUNT; i++) {
        bool sel = (i == selected);
        M5.Display.setTextColor(sel ? accent : fg, bg);
        M5.Display.setCursor(8, y);
        M5.Display.print(sel ? "> " : "  ");
        M5.Display.print(k85_bootloader_items_bios[i]);
        y += 12;
    }

    M5.Display.setTextColor(0x777777, bg);
    M5.Display.setCursor(4, M5.Display.height() - 20);
    if (seconds_left > 0) {
        M5.Display.printf("Auto-boot %ds", seconds_left);
    }
    M5.Display.setCursor(4, M5.Display.height() - 10);
    M5.Display.print("A=next B=select");
}

// ---------- GRUB-style ----------
static const char *k85_bootloader_items_grub[K85_BOOT_MENU_ITEM_COUNT] = {
    "k85OS", "k85OS (BIOS / recovery)", "Test Mode", "Alternate Firmware"
};

static void draw_boot_menu_grub(int selected, int seconds_left) {
    uint32_t bg = 0x000000;
    uint32_t fg = 0xFFFFFF;
    uint32_t sel_bg = 0xFFFFFF;
    uint32_t sel_fg = 0x000000;

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x00AA00, bg);
    M5.Display.setCursor(4, 4);
    M5.Display.print("GNU k85GRUB  version 2.06-k85");

    int y = 24;
    int w = M5.Display.width();
    for (int i = 0; i < K85_BOOT_MENU_ITEM_COUNT; i++) {
        bool sel = (i == selected);
        if (sel) {
            M5.Display.fillRect(4, y - 1, w - 8, 11, sel_bg);
        }
        M5.Display.setTextColor(sel ? sel_fg : fg, sel ? sel_bg : bg);
        M5.Display.setCursor(8, y);
        M5.Display.print(k85_bootloader_items_grub[i]);
        y += 13;
    }

    M5.Display.setTextColor(0x888888, bg);
    M5.Display.setCursor(4, M5.Display.height() - 20);
    M5.Display.print("Use A to move, B to boot");
    if (seconds_left > 0) {
        M5.Display.setCursor(4, M5.Display.height() - 10);
        M5.Display.printf("booting in %ds...", seconds_left);
    }
}

// ---------- rEFInd-style: горизонтальная галерея боксов ----------
static const char *k85_bootloader_items_refind[K85_BOOT_MENU_ITEM_COUNT] = {
    "k85OS", "BIOS", "Test", "AltFW"
};

static void draw_boot_menu_refind(int selected, int seconds_left) {
    uint32_t bg = 0x101010;
    uint32_t fg = 0xEEEEEE;
    uint32_t accent = 0x3399FF;

    M5.Display.fillScreen(bg);
    int w = M5.Display.width();
    int h = M5.Display.height();

    int box_w = w / K85_BOOT_MENU_ITEM_COUNT;
    int box_h = h - 40;
    int top = 10;

    for (int i = 0; i < K85_BOOT_MENU_ITEM_COUNT; i++) {
        bool sel = (i == selected);
        int x = i * box_w;
        M5.Display.drawRoundRect(x + 3, top, box_w - 6, box_h, 6, sel ? accent : 0x444444);
        if (sel) {
            M5.Display.drawRoundRect(x + 4, top + 1, box_w - 8, box_h - 2, 5, accent);
        }
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(sel ? accent : fg, bg);
        const char *label = k85_bootloader_items_refind[i];
        int lw = (int)strlen(label) * 6;
        int tx = x + (box_w - lw) / 2;
        int ty = top + box_h / 2;
        M5.Display.setCursor(tx, ty);
        M5.Display.print(label);
    }

    M5.Display.setTextColor(0x888888, bg);
    M5.Display.setCursor(4, h - 12);
    M5.Display.print("A=next B=boot");
    if (seconds_left > 0) {
        M5.Display.setCursor(w - 40, h - 12);
        M5.Display.printf("%ds", seconds_left);
    }
}

// Диспетчер: выбирает draw-функцию по g_config.boot_loader_style.
// style 0 (k85OS Boot Menu, текущий) рисует draw_boot_menu() как раньше.
static void draw_boot_menu_dispatch(int selected, int seconds_left) {
    int style = g_config.boot_loader_style;
    switch (style) {
        case 1: draw_boot_menu_bios(selected, seconds_left); break;
        case 2: draw_boot_menu_grub(selected, seconds_left); break;
        case 3: draw_boot_menu_refind(selected, seconds_left); break;
        default: draw_boot_menu(selected, seconds_left); break;
    }
}

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
            draw_boot_menu_dispatch(selected, seconds_left);
            last_seconds_shown = seconds_left;
        }

        if (k85_btn_a_pressed()) {
            interacted = true;
            selected = (selected + 1) % K85_BOOT_MENU_ITEM_COUNT;
            draw_boot_menu_dispatch(selected, 0);
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

static const char *panic_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC:    return "Kernel panic (unhandled exception)";
        case ESP_RST_TASK_WDT: return "Task watchdog timeout";
        case ESP_RST_INT_WDT:  return "Interrupt watchdog timeout";
        case ESP_RST_WDT:      return "Other watchdog reset";
        case ESP_RST_BROWNOUT: return "Brownout (power supply fault)";
        default:                return "Unknown fault";
    }
}

void k85_check_panic_screen(void) {
    esp_reset_reason_t r = esp_reset_reason();
    bool is_fault = (r == ESP_RST_PANIC || r == ESP_RST_TASK_WDT || r == ESP_RST_INT_WDT ||
                      r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
    if (!is_fault) return;

    int H = M5.Display.height();
    M5.Display.fillScreen(0x000000);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xFF4444, 0x000000);
    M5.Display.setCursor(4, 4);
    M5.Display.print("*** KERNEL PANIC ***");

    M5.Display.setTextColor(0xCCCCCC, 0x000000);
    M5.Display.setCursor(4, 18);
    M5.Display.print("k85OS / FreeRTOS fault detected");

    M5.Display.setTextColor(0xFFFFFF, 0x000000);
    M5.Display.setCursor(4, 34);
    M5.Display.print("Reason:");
    M5.Display.setCursor(4, 46);
    M5.Display.print(panic_reason_str(r));

    M5.Display.setTextColor(0x888888, 0x000000);
    M5.Display.setCursor(4, H - 20);
    M5.Display.print("Device recovered and rebooted.");
    M5.Display.setCursor(4, H - 10);
    M5.Display.print("Press A+B to continue booting");

    int64_t start_us = esp_timer_get_time();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        if (esp_timer_get_time() - start_us > 15000000) break; // авто-продолжение через 15с
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_show_boot_screen(void) {
    s_boot_theme = k85_boot_theme_load();

    show_pre_boot_logo(s_boot_theme);

    BootChoice choice;
    if (g_config.grub_enabled) {
        choice = run_boot_menu();
    } else {
        int c = g_config.default_boot_choice;
        if (c < 0 || c > 3) c = 0;
        choice = (BootChoice)c;
    }

    if (choice == BOOT_BIOS) {
        k85_run_bios_menu();
        return;
    }
    if (choice == BOOT_TEST) {
        k85_run_test_mode();
        return;
    }
    if (choice == BOOT_ALT_FW) {
        const esp_partition_t *alt = esp_ota_get_next_update_partition(nullptr);
        if (alt) {
            esp_ota_set_boot_partition(alt);
            esp_restart();
        }
        // РµСЃР»Рё СЃРІРѕР±РѕРґРЅРѕРіРѕ СЃР»РѕС‚Р° РЅРµС‚/РїСѓСЃС‚ вЂ” РїР°РґР°РµРј РІ РѕР±С‹С‡РЅСѓСЋ Р·Р°РіСЂСѓР·РєСѓ РЅРёР¶Рµ
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
