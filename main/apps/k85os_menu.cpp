#include "k85os_menu.h"
#include "common.h"
#include "theme.h"
#include "battery.h"
#include "sound.h"
#include "input.h"
#include "config.h"
#include "wifi.h"
#include "list_menu.h"
#include "../core/post_beep.h"
#include "../core/bios_theme.h"
#include "../core/version.h"
#include "../net/app_repo.h"

#include "M5Unified.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

using namespace k85;

#define K85_BIOS_ITEM_COUNT 21
#define K85_BIOS_VISIBLE_ROWS 6

static const char *k85_bios_labels[K85_BIOS_ITEM_COUNT] = {
    "WiFi module", "Bluetooth module", "OTA lock (soft)",
    "RAM / ROM info", "Chip info", "Uptime",
    "Active OTA slot", "Rollback firmware", "MAC address", "Battery voltage",
    "Update UEFI theme", "Customization",
    "POST beep", "POST beep info", "Mute all sound", "GRUB menu", "Boot options",
    "Wipe WiFi networks", "Reset config", "Factory reset",
    "Reboot",
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
    static const char *items[] = {"BG color", "Highlight color", "Text color", "Selection style", "Back"};
    int selected = 0;

    while (true) {
        char labeled[5][32];
        int bg_idx = color_preset_index(g_config.bios_bg_color);
        int hl_idx = color_preset_index(g_config.bios_hl_color);
        int tx_idx = color_preset_index(g_config.bios_text_color);

        snprintf(labeled[0], sizeof(labeled[0]), "BG: %s", bg_idx < 0 ? "Default" : K85_BIOS_COLOR_NAMES[bg_idx]);
        snprintf(labeled[1], sizeof(labeled[1]), "Highlight: %s", hl_idx < 0 ? "Default" : K85_BIOS_COLOR_NAMES[hl_idx]);
        snprintf(labeled[2], sizeof(labeled[2]), "Text: %s", tx_idx < 0 ? "Default" : K85_BIOS_COLOR_NAMES[tx_idx]);
        snprintf(labeled[3], sizeof(labeled[3]), "Select: %s", g_config.bios_selection_style == 0 ? "Filled" : "Arrow");
        snprintf(labeled[4], sizeof(labeled[4]), "Back");
        const char *display[5] = { labeled[0], labeled[1], labeled[2], labeled[3], labeled[4] };

        int idx = k85_run_list_menu("CUSTOMIZATION", display, 5, nullptr);
        if (idx < 0 || idx == 4) return;

        if (idx == 3) {
            g_config.bios_selection_style = (g_config.bios_selection_style + 1) % 2;
            k85_config_save();
            continue;
        }

        // Р¦РёРєР»РёС‡РµСЃРєРёР№ РІС‹Р±РѕСЂ: Default -> 10 С†РІРµС‚РѕРІ -> Default...
        uint32_t *target = (idx == 0) ? &g_config.bios_bg_color : (idx == 1) ? &g_config.bios_hl_color : &g_config.bios_text_color;
        int cur = color_preset_index(*target);
        cur++;
        if (cur >= 10) {
            *target = 0xFFFFFFFF; // РЅР°Р·Р°Рґ Рє РґРµС„РѕР»С‚Сѓ
        } else {
            *target = K85_BIOS_COLOR_PRESETS[cur];
        }
        k85_config_save();
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

static void bios_draw(void) {
    bios_clamp_scroll();

    bool custom_bg = (g_config.bios_bg_color != 0xFFFFFFFF);
    uint32_t base_bg = custom_bg ? g_config.bios_bg_color : s_bios_theme.bg;
    uint32_t accent = (g_config.bios_hl_color != 0xFFFFFFFF) ? g_config.bios_hl_color : s_bios_theme.accent;
    uint32_t fg = (g_config.bios_text_color != 0xFFFFFFFF) ? g_config.bios_text_color : s_bios_theme.fg;

    uint32_t grad_top, grad_bottom;
    if (custom_bg) {
        grad_top = base_bg;
        grad_bottom = base_bg; // СЃРїР»РѕС€РЅРѕР№ С†РІРµС‚, Р±РµР· РіСЂР°РґРёРµРЅС‚Р°
    } else {
        grad_top = darken(base_bg == 0x000000 ? 0x0000C0 : base_bg, 60);
        grad_bottom = (base_bg == 0x000000) ? 0x0000C0 : base_bg;
    }
    draw_gradient_bg(grad_top, grad_bottom);
    draw_uefi_border();

    uint32_t header_fg = contrast_color(grad_top);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(6, 4);
    M5.Display.setTextColor(header_fg, grad_top);
    M5.Display.print("k85OS Setup Utility");
    char ver[16];
    snprintf(ver, sizeof(ver), "v%s", K85_FW_VERSION);
    int w = M5.Display.width();
    M5.Display.setCursor(w - (int)strlen(ver) * 6 - 6, 4);
    M5.Display.print(ver);

    M5.Display.drawFastHLine(2, 14, w - 4, 0x5555AA);

    bool filled_style = (g_config.bios_selection_style == 0);
    uint32_t sel_text_color = contrast_color(accent);

    int y = 22;
    char val[32];
    int last_visible = s_scroll_top + K85_BIOS_VISIBLE_ROWS;
    if (last_visible > K85_BIOS_ITEM_COUNT) last_visible = K85_BIOS_ITEM_COUNT;

    for (int i = s_scroll_top; i < last_visible; i++) {
        bool sel = (i == s_selected);
        bool danger = (i == 19); // Factory reset

        uint32_t row_bg = (custom_bg ? base_bg : grad_bottom);
        if (sel && filled_style) {
            M5.Display.fillRect(2, y - 2, w - 4, 13, accent);
        }

        M5.Display.setCursor(6, y);
        uint32_t item_fg;
        uint32_t item_bg;
        if (sel && filled_style) {
            item_fg = sel_text_color;
            item_bg = accent;
        } else if (sel && !filled_style) {
            item_fg = accent; // РІС‹РґРµР»РµРЅРёРµ С‚РѕР»СЊРєРѕ С†РІРµС‚РѕРј С‚РµРєСЃС‚Р° РїСЂРё "СЃС‚СЂРµР»РѕС‡РЅРѕРј" СЃС‚РёР»Рµ
            item_bg = row_bg;
        } else {
            item_fg = danger ? 0xFF4444 : fg;
            item_bg = row_bg;
        }
        M5.Display.setTextColor(item_fg, item_bg);
        M5.Display.print(sel ? "> " : "  ");
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
            if (confirm_action("Reboot device")) {
                esp_restart();
            }
            break;
        default:
            break;
    }
}

void k85_run_bios_menu(void) {
    s_selected = 0;
    s_scroll_top = 0;
    s_bios_theme = k85_bios_theme_load();
    bios_draw();

    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
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

