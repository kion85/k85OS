#include "k85os_menu.h"
#include "common.h"
#include "theme.h"
#include "battery.h"
#include "power.h"
#include "sound.h"
#include "input.h"
#include "config.h"
#include "wifi.h"
#include "list_menu.h"
#include "../core/post_beep.h"
#include "../core/bios_theme.h"
#include "../core/boot_screen.h"
#include "../core/version.h"
#include "../net/app_repo.h"
#include "../core/cursor.h"

#include "M5Unified.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

using namespace k85;

#define K85_BIOS_ITEM_COUNT 22
#define K85_BIOS_VISIBLE_ROWS 6

static const char *k85_bios_labels[K85_BIOS_ITEM_COUNT] = {
    "WiFi module", "Bluetooth module", "OTA lock (soft)",
    "RAM / ROM info", "Chip info", "Uptime",
    "Active OTA slot", "Rollback firmware", "MAC address", "Battery voltage",
    "Update UEFI theme", "Customization",
    "POST beep", "POST beep info", "Mute all sound", "GRUB menu", "Boot options",
    "Wipe WiFi networks", "Reset config", "Factory reset",
    "Boot loader style", "Reboot",
};

static int s_selected = 0;
static int s_scroll_top = 0;

static void bios_clamp_scroll(void) {
    if (s_selected < s_scroll_top) s_scroll_top = s_selected;
    if (s_selected >= s_scroll_top + K85_BIOS_VISIBLE_ROWS) s_scroll_top = s_selected - K85_BIOS_VISIBLE_ROWS + 1;
    if (s_scroll_top < 0) s_scroll_top = 0;
    int max_top = K85_BIOS_ITEM_COUNT - K85_BIOS_VISIBLE_ROWS;
    if (max_top < 0) max_top = 0;
    if (s_scroll_top > max_top) s_scroll_top = max_top;
}

static void bios_value_str(char *out, size_t out_size, int idx) {
    switch (idx) {
        case 0: snprintf(out, out_size, "%s", g_config.wifi_disabled ? "OFF" : "ON"); break;
        case 1: snprintf(out, out_size, "%s", g_config.bt_disabled ? "OFF" : "ON"); break;
        case 2: snprintf(out, out_size, "%s", g_config.ota_locked ? "LOCKED" : "unlocked"); break;
        case 6: {
            const esp_partition_t *p = esp_ota_get_running_partition();
            snprintf(out, out_size, "%s", p ? p->label : "?");
            break;
        }
        case 12: snprintf(out, out_size, "%s", g_config.post_beep_enabled ? "ON" : "OFF"); break;
        case 14: snprintf(out, out_size, "%s", g_config.sound_muted ? "MUTED" : "unmuted"); break;
        case 15: snprintf(out, out_size, "%s", g_config.grub_enabled ? "ON" : "OFF"); break;
        case 16: {
            static const char *names[3] = {"Normal", "BIOS", "Test Mode"};
            int c = g_config.default_boot_choice;
            if (c < 0 || c > 2) c = 0;
            snprintf(out, out_size, "%s", names[c]);
            break;
        }
        case 20: {
            int s = g_config.boot_loader_style;
            if (s < 0 || s >= K85_BOOT_LOADER_STYLE_COUNT) s = 0;
            snprintf(out, out_size, "%s", k85_boot_loader_style_names[s]);
            break;
        }
        default: out[0] = 0;
    }
}

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void show_ram_rom_info(void) {
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);
    unsigned used_kb = (unsigned)(heap_info.total_allocated_bytes / 1024);
    unsigned total_kb = (unsigned)((heap_info.total_allocated_bytes + heap_info.total_free_bytes) / 1024);

    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);

    size_t fs_total = 0, fs_used = 0;
    bool fs_ok = k85_fs_info(&fs_total, &fs_used);

    char msg[160];
    if (fs_ok) {
        snprintf(msg, sizeof(msg),
            "RAM: %u/%u KB\nFlash: %u MB\nLittleFS: %u/%u KB\nA+B=back",
            used_kb, total_kb, (unsigned)(flash_size / (1024 * 1024)),
            (unsigned)(fs_used / 1024), (unsigned)(fs_total / 1024));
    } else {
        snprintf(msg, sizeof(msg),
            "RAM: %u/%u KB\nFlash: %u MB\nA+B=back",
            used_kb, total_kb, (unsigned)(flash_size / (1024 * 1024)));
    }
    k85_show_message(msg);
    wait_ab_exit();
}

static void show_chip_info(void) {
    esp_chip_info_t info;
    esp_chip_info(&info);
    char msg[128];
    snprintf(msg, sizeof(msg),
        "Model: ESP32-S3\nRev: v%d.%d\nCores: %d\nFreq: %d MHz\nA+B=back",
        info.revision / 100, info.revision % 100, info.cores, 240);
    k85_show_message(msg);
    wait_ab_exit();
}

static void show_uptime(void) {
    int64_t us = esp_timer_get_time();
    int64_t s = us / 1000000;
    int h = (int)(s / 3600);
    int m = (int)((s % 3600) / 60);
    int sec = (int)(s % 60);
    char msg[64];
    snprintf(msg, sizeof(msg), "Uptime: %02d:%02d:%02d\nA+B=back", h, m, sec);
    k85_show_message(msg);
    wait_ab_exit();
}

static void show_active_slot(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    char msg[128];
    snprintf(msg, sizeof(msg),
        "Running: %s\n@ 0x%06X\nOther slot: %s\nA+B=back",
        running ? running->label : "?", running ? (unsigned)running->address : 0,
        next ? next->label : "?");
    k85_show_message(msg);
    wait_ab_exit();
}

static void show_mac_address(void) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char msg[80];
    snprintf(msg, sizeof(msg), "STA MAC:\n%02X:%02X:%02X:%02X:%02X:%02X\nA+B=back",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    k85_show_message(msg);
    wait_ab_exit();
}

static void show_battery_voltage(void) {
    int32_t mv = M5.Power.getBatteryVoltage();
    char msg[64];
    if (mv > 0) {
        snprintf(msg, sizeof(msg), "Battery: %.2f V\nA+B=back", mv / 1000.0f);
    } else {
        snprintf(msg, sizeof(msg), "Battery: N/A\nA+B=back");
    }
    k85_show_message(msg);
    wait_ab_exit();
}

static void show_post_info(void) {
    k85_show_message(
        "POST beep codes:\n"
        "1 low+long: LittleFS fail\n"
        "2 high: RTC not found\n"
        "3 mid: battery low\n"
        "2 asc tones: OK, no errors\n"
        "A+B=back");
    wait_ab_exit();
}

static bool confirm_action(const char *label) {
    char msg[80];
    snprintf(msg, sizeof(msg), "%s?\nB=confirm A+B=cancel", label);
    k85_show_message(msg);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) return true;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void run_rollback(void) {
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    if (!next) {
        k85_show_message("No other slot found\nA+B=back");
        wait_ab_exit();
        return;
    }

    char msg[160];
    snprintf(msg, sizeof(msg),
        "Rollback to %s?\nWARNING: if that slot\nis empty/broken,\ndevice may hang.\nB=confirm A+B=cancel",
        next->label);
    k85_show_message(msg);

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_b_pressed()) break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    esp_err_t err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        k85_show_message("Rollback failed\nA+B=back");
        wait_ab_exit();
        return;
    }
    k85_show_message("Rolling back...\nRebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static K85BiosTheme s_bios_theme;

static void run_update_uefi_theme(void) {
    if (!k85_wifi_is_connected()) {
        k85_show_message("WiFi not connected\nA+B=back");
        wait_ab_exit();
        return;
    }

    k85_show_message("Fetching theme list...");

    static char names[K85_APPREPO_MAX_ASSETS][64];
    static char urls[K85_APPREPO_MAX_ASSETS][256];
    int count = 0;

    if (!k85_apprepo_fetch_uefi_theme_list(names, urls, K85_APPREPO_MAX_ASSETS, &count) || count == 0) {
        k85_show_message("No themes found\nin apps_k85os/uefi\nA+B=back");
        wait_ab_exit();
        return;
    }

    const char *items[K85_APPREPO_MAX_ASSETS + 1];
    for (int i = 0; i < count; i++) items[i] = names[i];
    items[count] = "Back";

    int idx = k85_run_list_menu("UEFI THEMES", items, count + 1, nullptr);
    if (idx < 0 || idx >= count) return;

    k85_show_message("Downloading...");

    mkdir("/littlefs/bios", 0755);
    char dest[192];
    snprintf(dest, sizeof(dest), "/littlefs/bios/%s", names[idx]);

    if (!k85_apprepo_download_file(urls[idx], dest)) {
        k85_show_message("Download failed\nA+B=back");
        wait_ab_exit();
        return;
    }

    FILE *fsrc = fopen(dest, "rb");
    FILE *fdst = fopen(K85_BIOS_THEME_ACTIVE_FILE, "wb");
    if (fsrc && fdst) {
        char buf[256];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), fsrc)) > 0) fwrite(buf, 1, r, fdst);
    }
    if (fsrc) fclose(fsrc);
    if (fdst) fclose(fdst);

    s_bios_theme = k85_bios_theme_load();

    // сбрасываем ручные overrides из Customization — иначе они перекрывают цвета новой темы
    g_config.bios_bg_color = 0xFFFFFFFF;
    g_config.bios_hl_color = 0xFFFFFFFF;
    g_config.bios_text_color = 0xFFFFFFFF;
    k85_config_save();

    char msg[80];
    snprintf(msg, sizeof(msg), "Applied: %.40s\nA+B=back", names[idx]);
    k85_show_message(msg);
    wait_ab_exit();
}

// ---------- Customization ----------
static const uint32_t K85_BIOS_COLOR_PRESETS[10] = {
    0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF,
    0xFFFF00, 0xFF00FF, 0x00FFFF, 0x808080, 0xFFA500,
};
static const char *K85_BIOS_COLOR_NAMES[10] = {
    "Black", "White", "Red", "Green", "Blue",
    "Yellow", "Magenta", "Cyan", "Gray", "Orange",
};

static int color_preset_index(uint32_t color) {
    for (int i = 0; i < 10; i++) if (K85_BIOS_COLOR_PRESETS[i] == color) return i;
    return -1;
}

static void run_customization_menu(void) {
    const int CUST_COUNT = 6;
    static int selected = 0;
    if (selected >= CUST_COUNT) selected = 0;

    char labeled[6][32];

    auto build_labels = [&]() {
        int bg_idx = color_preset_index(g_config.bios_bg_color);
        int hl_idx = color_preset_index(g_config.bios_hl_color);
        int tx_idx = color_preset_index(g_config.bios_text_color);
        snprintf(labeled[0], sizeof(labeled[0]), "BG: %s", bg_idx < 0 ? "Default" : K85_BIOS_COLOR_NAMES[bg_idx]);
        snprintf(labeled[1], sizeof(labeled[1]), "Highlight: %s", hl_idx < 0 ? "Default" : K85_BIOS_COLOR_NAMES[hl_idx]);
        snprintf(labeled[2], sizeof(labeled[2]), "Text: %s", tx_idx < 0 ? "Default" : K85_BIOS_COLOR_NAMES[tx_idx]);
        snprintf(labeled[3], sizeof(labeled[3]), "Select: %s", g_config.bios_selection_style == 0 ? "Filled" : "Arrow");
        static const char *ui_style_names[3] = {"List", "Grid", "List+Icons"};
        int uis = g_config.bios_ui_style;
        if (uis < 0 || uis > 2) uis = 0;
        snprintf(labeled[4], sizeof(labeled[4]), "UI Style: %s", ui_style_names[uis]);
        snprintf(labeled[5], sizeof(labeled[5]), "Back");
    };

    auto draw = [&]() {
        build_labels();
        uint32_t bg = k85_get_bg();
        uint32_t fg = k85_get_fg();
        uint32_t accent = k85_get_accent();
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(4, 2);
        M5.Display.setTextColor(accent, bg);
        M5.Display.print("CUSTOMIZATION");
        int y = 18;
        for (int i = 0; i < CUST_COUNT; i++) {
            bool sel = (i == selected);
            M5.Display.setCursor(4, y);
            M5.Display.setTextColor(sel ? (uint32_t)0x000000 : fg, sel ? accent : bg);
            M5.Display.print(sel ? "> " : "  ");
            M5.Display.print(labeled[i]);
            y += 14;
        }
        M5.Display.setTextColor((uint32_t)0xAAAAAA, bg);
        M5.Display.setCursor(4, y + 4);
        M5.Display.print("A=next B=select A+B=exit");
    };

    draw();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed()) {
            selected = (selected + 1) % CUST_COUNT;
            draw();
        }
        if (k85_btn_b_pressed()) {
            if (selected == 5) { return; }
            if (selected == 4) {
                g_config.bios_ui_style = (g_config.bios_ui_style + 1) % 3;
                k85_config_save();
            } else if (selected == 3) {
                g_config.bios_selection_style = (g_config.bios_selection_style + 1) % 2;
                k85_config_save();
            } else {
                uint32_t *target = (selected == 0) ? &g_config.bios_bg_color : (selected == 1) ? &g_config.bios_hl_color : &g_config.bios_text_color;
                int cur = color_preset_index(*target);
                cur++;
                if (cur >= 10) {
                    *target = 0xFFFFFFFF;
                } else {
                    *target = K85_BIOS_COLOR_PRESETS[cur];
                }
                k85_config_save();
            }
            draw();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}


static uint32_t darken(uint32_t color, int percent) {
    int r = (color >> 16) & 0xFF;
    int g = (color >> 8) & 0xFF;
    int b = color & 0xFF;
    r = r * (100 - percent) / 100;
    g = g * (100 - percent) / 100;
    b = b * (100 - percent) / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Р’С‹Р±РёСЂР°РµС‚ С‡С‘СЂРЅС‹Р№ РёР»Рё Р±РµР»С‹Р№ С‚РµРєСЃС‚ РІ Р·Р°РІРёСЃРёРјРѕСЃС‚Рё РѕС‚ СЏСЂРєРѕСЃС‚Рё С„РѕРЅР° (РєРѕРЅС‚СЂР°СЃС‚ РІСЃРµРіРґР° С‡РёС‚Р°РµРј)
static uint32_t contrast_color(uint32_t bg_color) {
    int r = (bg_color >> 16) & 0xFF;
    int g = (bg_color >> 8) & 0xFF;
    int b = bg_color & 0xFF;
    int luminance = (r * 299 + g * 587 + b * 114) / 1000;
    return luminance > 140 ? 0x000000 : 0xFFFFFF;
}

static void draw_gradient_bg(uint32_t top_color, uint32_t bottom_color) {
    int h = M5.Display.height();
    int w = M5.Display.width();
    int tr = (top_color >> 16) & 0xFF, tg = (top_color >> 8) & 0xFF, tb = top_color & 0xFF;
    int br = (bottom_color >> 16) & 0xFF, bg_ = (bottom_color >> 8) & 0xFF, bb = bottom_color & 0xFF;

    for (int y = 0; y < h; y++) {
        float t = (float)y / (float)h;
        int r = tr + (int)((br - tr) * t);
        int g = tg + (int)((bg_ - tg) * t);
        int b = tb + (int)((bb - tb) * t);
        uint32_t row_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        M5.Display.drawFastHLine(0, y, w, row_color);
    }
}

static void draw_uefi_border(void) {
    int w = M5.Display.width();
    int h = M5.Display.height();
    uint32_t light = 0x8080FF;
    uint32_t dark = 0x000020;

    M5.Display.drawFastHLine(0, 0, w, light);
    M5.Display.drawFastVLine(0, 0, h, light);
    M5.Display.drawFastHLine(0, h - 1, w, dark);
    M5.Display.drawFastVLine(w - 1, 0, h, dark);
}

// idx соответствует k85_bios_labels[idx] — уникальная иконка на каждый из 21 пункта.
static void draw_bios_icon(int idx, int cx, int cy, int r, uint32_t col) {
    auto &d = M5.Display;
    switch (idx) {
        case 0: // WiFi module
            d.fillRect(cx - 1, cy + r/2 - 1, 2, 2, col);
            d.drawLine(cx - r/2, cy, cx, cy - r/2, col);
            d.drawLine(cx, cy - r/2, cx + r/2, cy, col);
            break;
        case 1: // Bluetooth module
            d.drawLine(cx, cy - r, cx, cy + r, col);
            d.drawLine(cx, cy - r, cx + r/2, cy - r/3, col);
            d.drawLine(cx + r/2, cy - r/3, cx - r/2, cy + r/3, col);
            d.drawLine(cx - r/2, cy + r/3, cx + r/2, cy + r - 2, col);
            d.drawLine(cx + r/2, cy + r - 2, cx, cy + r, col);
            break;
        case 2: // OTA lock
            d.drawRoundRect(cx - r/2, cy - 1, r, r/2 + 2, 1, col);
            d.drawCircle(cx, cy - r/2, r/3, col);
            break;
        case 3: // RAM/ROM info
            d.drawRect(cx - r/2, cy - r/3, r, r*2/3, col);
            for (int k = -1; k <= 1; k++) d.drawFastVLine(cx + k * r/3, cy - r/3 - 2, 2, col);
            break;
        case 4: // Chip info
            d.drawRect(cx - r/3, cy - r/3, r*2/3, r*2/3, col);
            d.drawFastHLine(cx - r/2, cy, r/6, col);
            d.drawFastHLine(cx + r/3, cy, r/6, col);
            d.drawFastVLine(cx, cy - r/2, r/6, col);
            d.drawFastVLine(cx, cy + r/3, r/6, col);
            break;
        case 5: // Uptime
            d.drawCircle(cx, cy, r, col);
            d.drawLine(cx, cy, cx, cy - r + 2, col);
            d.drawLine(cx, cy, cx + r/2, cy, col);
            break;
        case 6: // Active OTA slot
            d.drawRoundRect(cx - r/2, cy - r/2, r, r, 2, col);
            d.drawFastHLine(cx - r/2 + 1, cy, r - 2, col);
            break;
        case 7: // Rollback firmware
            d.drawCircle(cx, cy, r/2, col);
            d.fillTriangle(cx - r/2 - 2, cy - 1, cx - r/2 - 2, cy + 3, cx - r/2 + 2, cy + 1, col);
            break;
        case 8: // MAC address
            d.drawFastHLine(cx - r/2, cy - 2, r, col);
            d.drawFastHLine(cx - r/2, cy + 2, r, col);
            d.drawFastVLine(cx - r/4, cy - r/2, r, col);
            d.drawFastVLine(cx + r/4, cy - r/2, r, col);
            break;
        case 9: // Battery voltage
            d.drawRect(cx - r/2, cy - r/3, r - 1, r*2/3, col);
            d.fillRect(cx + r/2 - 1, cy - 2, 2, 4, col);
            d.fillRect(cx - r/2 + 2, cy - r/3 + 2, r - 5, r*2/3 - 4, col);
            break;
        case 10: // Update UEFI theme
            d.drawLine(cx, cy - r/2, cx, cy + r/3, col);
            d.fillTriangle(cx - 3, cy, cx + 3, cy, cx, cy + r/2, col);
            break;
        case 11: // Customization
            d.fillCircle(cx - r/3, cy - r/3, 2, col);
            d.fillCircle(cx + r/3, cy - r/3, 2, col);
            d.fillCircle(cx, cy + r/3, 2, col);
            d.drawCircle(cx, cy, r, col);
            break;
        case 12: // POST beep
            d.fillTriangle(cx - r/2, cy - r/3, cx - r/2, cy + r/3, cx - 1, cy, col);
            d.drawCircle(cx + r/3, cy, r/3, col);
            break;
        case 13: // POST beep info
            d.drawCircle(cx, cy, r, col);
            d.fillRect(cx - 1, cy - r/3, 2, r/3, col);
            d.fillRect(cx - 1, cy - r/2 - 2, 2, 2, col);
            break;
        case 14: // Mute all sound
            d.fillTriangle(cx - r/2, cy - r/3, cx - r/2, cy + r/3, cx - 1, cy, col);
            d.drawLine(cx + r/4, cy - r/3, cx + r, cy + r/3, col);
            d.drawLine(cx + r/4, cy + r/3, cx + r, cy - r/3, col);
            break;
        case 15: // GRUB menu
            d.drawFastHLine(cx - r/2, cy - r/2, r, col);
            d.drawFastHLine(cx - r/2, cy, r, col);
            d.drawFastHLine(cx - r/2, cy + r/2, r, col);
            break;
        case 16: // Boot options
            d.drawCircle(cx, cy, r/2, col);
            d.drawFastVLine(cx, cy - r, r/2, col);
            break;
        case 17: // Wipe WiFi networks
            d.drawLine(cx - r/2, cy - r/2, cx, cy - r/2, col);
            d.drawLine(cx, cy - r/2, cx, cy - 1, col);
            d.drawLine(cx - r/2, cy + r/2, cx + r/2, cy - r/2, col);
            break;
        case 18: // Reset config
            d.drawCircle(cx, cy, r/2, col);
            d.fillTriangle(cx + r/2 - 1, cy - r/2, cx + r/2 + 3, cy - r/2, cx + r/2 + 1, cy - r/2 + 4, col);
            break;
        case 19: // Factory reset (danger)
            d.fillTriangle(cx - r/2, cy + r/2, cx, cy - r/2, cx + r/2, cy + r/2, col);
            break;
        case 20: // Boot loader style
            d.drawRoundRect(cx - r/2, cy - r/3, r, r*2/3, 1, col);
            d.drawFastHLine(cx - r/2 + 1, cy, r - 2, col);
            d.fillTriangle(cx + r/2 - 2, cy - r/3 - 1, cx + r/2 + 2, cy - r/3 - 1, cx + r/2, cy - r/3 - 4, col);
            break;
        case 21: // Reboot
            d.drawCircle(cx, cy, r/2, col);
            d.drawFastVLine(cx, cy - r/2 - 1, r/2, col);
            break;
        default:
            d.fillCircle(cx, cy, r/3, col);
    }
}
static void bios_draw_header(uint32_t grad_top, uint32_t w) {
    uint32_t header_fg = contrast_color(grad_top);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(6, 4);
    M5.Display.setTextColor(header_fg, grad_top);
    M5.Display.print("k85OS Setup Utility");
    char ver[16];
    snprintf(ver, sizeof(ver), "v%s", K85_FW_VERSION);
    M5.Display.setCursor((int)w - (int)strlen(ver) * 6 - 6, 4);
    M5.Display.print(ver);
    M5.Display.drawFastHLine(2, 14, (int)w - 4, 0x5555AA);
}

static void bios_draw_list(bool with_icons) {
    bios_clamp_scroll();

    bool custom_bg = (g_config.bios_bg_color != 0xFFFFFFFF);
    uint32_t base_bg = custom_bg ? g_config.bios_bg_color : s_bios_theme.bg;
    uint32_t accent = (g_config.bios_hl_color != 0xFFFFFFFF) ? g_config.bios_hl_color : s_bios_theme.accent;
    uint32_t fg = (g_config.bios_text_color != 0xFFFFFFFF) ? g_config.bios_text_color : s_bios_theme.fg;

    uint32_t grad_top, grad_bottom;
    if (custom_bg) {
        grad_top = base_bg;
        grad_bottom = base_bg;
    } else {
        grad_top = darken(base_bg == 0x000000 ? 0x0000C0 : base_bg, 60);
        grad_bottom = (base_bg == 0x000000) ? 0x0000C0 : base_bg;
    }
    draw_gradient_bg(grad_top, grad_bottom);
    draw_uefi_border();

    int w = M5.Display.width();
    bios_draw_header(grad_top, (uint32_t)w);

    bool filled_style = (g_config.bios_selection_style == 0);
    uint32_t sel_text_color = contrast_color(accent);

    int y = 22;
    char val[32];
    int last_visible = s_scroll_top + K85_BIOS_VISIBLE_ROWS;
    if (last_visible > K85_BIOS_ITEM_COUNT) last_visible = K85_BIOS_ITEM_COUNT;

    for (int i = s_scroll_top; i < last_visible; i++) {
        bool sel = (i == s_selected);
        bool danger = (i == 19);
        uint32_t row_bg = (custom_bg ? base_bg : grad_bottom);
        if (sel && filled_style) {
            M5.Display.fillRoundRect(2, y - 2, w - 4, 13, 4, accent);
        }
        uint32_t item_fg;
        uint32_t item_bg;
        if (sel && filled_style) {
            item_fg = sel_text_color;
            item_bg = accent;
        } else if (sel && !filled_style) {
            item_fg = accent;
            item_bg = row_bg;
        } else {
            item_fg = danger ? 0xFF4444 : fg;
            item_bg = row_bg;
        }
        M5.Display.setTextColor(item_fg, item_bg);
        if (with_icons) {
            uint32_t icon_col = danger ? 0xFF4444 : item_fg;
            draw_bios_icon(i, 11, y + 4, 5, icon_col);
            M5.Display.setCursor(24, y);
        } else {
            M5.Display.setCursor(6, y);
            M5.Display.print(sel ? "> " : "  ");
        }
        M5.Display.print(k85_bios_labels[i]);
        bios_value_str(val, sizeof(val), i);
        if (val[0]) {
            M5.Display.setCursor(150, y);
            M5.Display.setTextColor(sel ? item_fg : 0xAAAAAA, item_bg);
            M5.Display.print(val);
        }
        y += 14;
    }
    M5.Display.drawFastHLine(2, y + 2, w - 4, 0x5555AA);
    M5.Display.setTextColor(contrast_color(grad_bottom), grad_bottom);
    M5.Display.setCursor(6, y + 8);
    M5.Display.print("A=next B=select A+B=exit");

    k85_draw_battery_icon();
}

static void bios_draw_grid(void) {
    if (s_selected >= K85_BIOS_ITEM_COUNT) s_selected = K85_BIOS_ITEM_COUNT - 1;
    if (s_selected < 0) s_selected = 0;

    bool custom_bg = (g_config.bios_bg_color != 0xFFFFFFFF);
    uint32_t base_bg = custom_bg ? g_config.bios_bg_color : s_bios_theme.bg;
    uint32_t accent = (g_config.bios_hl_color != 0xFFFFFFFF) ? g_config.bios_hl_color : s_bios_theme.accent;
    uint32_t fg = (g_config.bios_text_color != 0xFFFFFFFF) ? g_config.bios_text_color : s_bios_theme.fg;

    uint32_t grad_top = custom_bg ? base_bg : darken(base_bg == 0x000000 ? 0x0000C0 : base_bg, 60);
    uint32_t grad_bottom = custom_bg ? base_bg : ((base_bg == 0x000000) ? 0x0000C0 : base_bg);
    draw_gradient_bg(grad_top, grad_bottom);
    draw_uefi_border();

    int w = M5.Display.width();
    int h = M5.Display.height();
    bios_draw_header(grad_top, (uint32_t)w);

    const int cols = 4;
    const int rows = 2;
    const int per_page = cols * rows;
    const int start_y = 18;
    int grid_h = h - start_y - 12;
    int cell_w = w / cols;
    int cell_h = grid_h / rows;

    int page = s_selected / per_page;
    int page_count = (K85_BIOS_ITEM_COUNT + per_page - 1) / per_page;
    int page_start = page * per_page;
    int page_end = page_start + per_page;
    if (page_end > K85_BIOS_ITEM_COUNT) page_end = K85_BIOS_ITEM_COUNT;

    for (int i = page_start; i < page_end; i++) {
        int local = i - page_start;
        int col = local % cols;
        int row = local / cols;
        int cx = col * cell_w + cell_w / 2;
        int cy = start_y + row * cell_h + cell_h / 2 - 6;
        bool sel = (i == s_selected);
        bool danger = (i == 19);
        if (sel) {
            M5.Display.fillRoundRect(col * cell_w + 3, start_y + row * cell_h + 2,
                                      cell_w - 6, cell_h - 4, 6, accent);
        }
        uint32_t icon_col = sel ? contrast_color(accent) : (danger ? 0xFF4444 : fg);
        draw_bios_icon(i, cx, cy, 10, icon_col);

        M5.Display.setTextColor(sel ? contrast_color(accent) : fg, sel ? accent : (custom_bg ? base_bg : grad_bottom));
        char short_label[16];
        int max_chars = (cell_w - 4) / 6;
        if (max_chars > 15) max_chars = 15;
        if (max_chars < 1) max_chars = 1;
        snprintf(short_label, sizeof(short_label), "%.*s", max_chars, k85_bios_labels[i]);
        int tx = col * cell_w + (cell_w - (int)strlen(short_label) * 6) / 2;
        if (tx < col * cell_w) tx = col * cell_w + 1;
        M5.Display.setCursor(tx, start_y + row * cell_h + cell_h - 12);
        M5.Display.print(short_label);
    }

    M5.Display.setTextColor(0xAAAAAA, custom_bg ? base_bg : grad_bottom);
    if (page_count > 1) {
        char pg[16];
        snprintf(pg, sizeof(pg), "%d/%d", page + 1, page_count);
        M5.Display.setCursor(w - (int)strlen(pg) * 6 - 4, h - 10);
        M5.Display.print(pg);
    }

    k85_draw_battery_icon();
}

static void bios_draw(void) {
    switch (g_config.bios_ui_style) {
        case 1: bios_draw_grid(); break;
        case 2: bios_draw_list(true); break;
        default: bios_draw_list(false); break;
    }
}

static void bios_apply(int idx) {
    switch (idx) {
        case 0:
            g_config.wifi_disabled = !g_config.wifi_disabled;
            if (g_config.wifi_disabled) k85_wifi_disconnect();
            k85_config_save();
            break;
        case 1:
            g_config.bt_disabled = !g_config.bt_disabled;
            k85_config_save();
            break;
        case 2:
            g_config.ota_locked = !g_config.ota_locked;
            k85_config_save();
            break;
        case 3: show_ram_rom_info(); break;
        case 4: show_chip_info(); break;
        case 5: show_uptime(); break;
        case 6: show_active_slot(); break;
        case 7: run_rollback(); break;
        case 8: show_mac_address(); break;
        case 9: show_battery_voltage(); break;
        case 10: run_update_uefi_theme(); break;
        case 11: run_customization_menu(); break;
        case 12:
            g_config.post_beep_enabled = !g_config.post_beep_enabled;
            k85_post_set_enabled(g_config.post_beep_enabled);
            k85_config_save();
            break;
        case 13: show_post_info(); break;
        case 14:
            g_config.sound_muted = !g_config.sound_muted;
            k85_apply_sound_volume();
            k85_config_save();
            break;
        case 15:
            g_config.grub_enabled = !g_config.grub_enabled;
            k85_config_save();
            break;
        case 16:
            g_config.default_boot_choice = (g_config.default_boot_choice + 1) % 3;
            k85_config_save();
            break;
        case 17:
            if (confirm_action("Wipe WiFi networks")) {
                g_config.wifi_saved = false;
                g_config.wifi_ssid[0] = 0;
                g_config.wifi_password[0] = 0;
                g_config.wifi_networks_count = 0;
                k85_config_save();
                k85_show_message("WiFi networks wiped\nA+B=back");
                wait_ab_exit();
            }
            break;
        case 18:
            if (confirm_action("Reset config to defaults")) {
                k85_config_defaults(&g_config);
                k85_config_save();
                k85_show_message("Config reset\nA+B=back");
                wait_ab_exit();
            }
            break;
        case 19:
            if (confirm_action("FACTORY RESET (wipe all data)")) {
                k85_config_defaults(&g_config);
                k85_config_save();
                esp_littlefs_format(K85_LITTLEFS_PART_LABEL);
                k85_show_message("Factory reset done\nRebooting...");
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
            break;
        case 20:
            k85_boot_loader_style_cycle();
            k85_config_save();
            break;
        case 21:
            if (confirm_action("Reboot device")) {
                esp_restart();
            }
            break;
        default:
            break;
    }
}

#define K85_BIOS_HOVER_DWELL_MS 120

// Хит-тест под реальную геометрию bios_draw_list()/bios_draw_grid().
static int bios_hit_test(void) {
    int style = g_config.bios_ui_style;
    int w = M5.Display.width();
    int h = M5.Display.height();
    int cx = k85_cursor_x();
    int cy = k85_cursor_y();

    if (style == 1) {
        const int cols = 4, rows = 2, per_page = cols * rows;
        const int start_y = 18;
        int grid_h = h - start_y - 12;
        int cell_w = w / cols;
        int cell_h = grid_h / rows;
        if (cy < start_y || cy >= start_y + grid_h) return -1;
        int col = cx / cell_w;
        int row = (cy - start_y) / cell_h;
        if (col < 0 || col >= cols || row < 0 || row >= rows) return -1;
        int page = s_selected / per_page;
        int page_start = page * per_page;
        int idx = page_start + row * cols + col;
        if (idx >= K85_BIOS_ITEM_COUNT) return -1;
        return idx;
    } else {
        bios_clamp_scroll();
        const int start_y = 22;
        const int line_h = 14;
        if (cy < start_y) return -1;
        int local = (cy - start_y) / line_h;
        if (local < 0 || local >= K85_BIOS_VISIBLE_ROWS) return -1;
        int idx = s_scroll_top + local;
        if (idx >= K85_BIOS_ITEM_COUNT) return -1;
        return idx;
    }
}

void k85_run_bios_menu(void) {
    s_selected = 0;
    s_scroll_top = 0;
    s_bios_theme = k85_bios_theme_load();
    bios_draw();

    bool cursor_mode = k85_cursor_active();
    if (cursor_mode) k85_cursor_reset();
    int pending_hover = -1;
    int64_t pending_hover_since = 0;

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }

        if (cursor_mode) {
            k85_cursor_update();
            int hover = bios_hit_test();
            if (hover >= 0) {
                int64_t now = esp_timer_get_time();
                if (hover != pending_hover) {
                    pending_hover = hover;
                    pending_hover_since = now;
                } else if ((now - pending_hover_since) >= K85_BIOS_HOVER_DWELL_MS * 1000) {
                    s_selected = hover;
                }
            } else {
                pending_hover = -1;
            }
            bios_draw();
            k85_cursor_draw();

            if (k85_btn_a_pressed()) {
                k85_wake_screen();
                bios_apply(s_selected);
                bios_draw();
                k85_cursor_draw();
            }

            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        if (k85_btn_a_pressed()) {
            s_selected = (s_selected + 1) % K85_BIOS_ITEM_COUNT;
            bios_draw();
        }
        if (k85_btn_b_pressed()) {
            bios_apply(s_selected);
            bios_draw();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

