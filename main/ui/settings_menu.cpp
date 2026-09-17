#include "settings_menu.h"
#include "config.h"
#include "theme.h"
#include "power.h"
#include "power_menu.h"
#include "core/fonts.h"
#include "sound.h"
#include "input.h"
#include "device.h"
#include "step_counter.h"
#include "wifi.h"
#include "common.h"
#include "boot_screen.h"
#include "../net/ota.h"
#include "../core/version.h"
#include "text_input.h"
#include "status_bar_settings.h"
#include "lock_screen_settings.h"
#include "../core/lock_auth.h"
#include "../net/ssh_server.h"
#include "../net/firmware_flash.h"
#include "cpu_freq.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <cmath>

#define K85_SETTINGS_ITEM_COUNT 20
#define K85_SETTINGS_BACK_IDX   (K85_SETTINGS_ITEM_COUNT - 1)

static const char *k85_settings_labels[K85_SETTINGS_ITEM_COUNT] = {
    "Theme", "Brightness", "Battery mode", "Boot style",
    "Device name", "Sound volume", "WiFi", "Reset steps",
    "Check for updates", "Screen lock", "Status bar", "BG gradient", "Lock screen", "SSH Server", "Menu UI style", "CPU freq", "Power", "Font", "Keyboard nav", "Back",
};

static int s_selected = 0;
static int s_scroll_top = 0;
#define K85_SETTINGS_VISIBLE_ROWS 6

static void settings_value_str(char *out, size_t out_size, int idx) {
    switch (idx) {
        case 0: snprintf(out, out_size, "%s", k85_get_theme()->name); break;
        case 1: snprintf(out, out_size, "%d%%", g_config.brightness_active); break;
        case 2: snprintf(out, out_size, "%s", k85_battery_modes[g_config.battery_mode_idx]); break;
        case 3: {
            int bi = g_config.bootstyle_idx;
            if (bi < 0 || bi >= K85_BOOT_STYLE_COUNT) bi = 0;
            snprintf(out, out_size, "%s", k85_boot_style_names[bi]);
            break;
        }
        case 4: snprintf(out, out_size, "%s", k85_get_device_name()); break;
        case 5: snprintf(out, out_size, "%d%%", k85_get_sound_volume()); break;
        case 6:
            if (g_config.wifi_disabled) {
                out[0] = 0;
                break;
            }
            if (g_config.wifi_saved) {
                snprintf(out, out_size, "%s%s", g_config.wifi_ssid,
                         k85_wifi_is_connected() ? " (on)" : " (off)");
            } else {
                snprintf(out, out_size, "not set");
            }
            break;
        case 7: snprintf(out, out_size, "%d", g_config.step_count); break;
        case 8: snprintf(out, out_size, "v%s", K85_FW_VERSION); break;
        case 9: snprintf(out, out_size, "%s", g_config.lock_enabled ? "ON" : "OFF"); break;
        case 10: out[0] = 0; break;
        case 11: snprintf(out, out_size, "%s", g_config.bg_gradient_enabled ? "ON" : "OFF"); break;
        case 12: out[0] = 0; break;
        case 13: snprintf(out, out_size, "%s", g_config.ssh_enabled ? "ON" : "OFF"); break;
        case 14: {
            static const char *ui_style_names[3] = {"List", "Grid", "List+Icons"};
            int s = g_config.menu_ui_style;
            if (s < 0 || s > 2) s = 0;
            snprintf(out, out_size, "%s", ui_style_names[s]);
            break;
        }
        case 15: {
            int mhz = g_config.cpu_freq_mhz;
            if (mhz != 240 && mhz != 160 && mhz != 80) mhz = 160;
            snprintf(out, out_size, "%d MHz", mhz);
            break;
        }
        case 16: out[0] = 0; break; // Power (подменю, значение не показываем)
        case 17: snprintf(out, out_size, "%s", k85_font_names[g_config.font_idx >= 0 && g_config.font_idx < K85_FONT_COUNT ? g_config.font_idx : 0]); break;
        case 18: snprintf(out, out_size, "%s", g_config.kbd_nav_mode == 1 ? "IMU (tilt)" : "Classic"); break;
        case 19: out[0] = 0; break; // Back
        default: out[0] = 0;
    }
}

// Иконки для Grid / List+Icons режимов Settings.
static void draw_settings_icon(int cx, int cy, int r, const char *name, uint32_t col, uint32_t bg_col) {
    auto &d = M5.Display;
    if (!strcmp(name, "Theme")) {
        d.fillCircle(cx, cy, r, col);
        d.fillArc(cx, cy, 0, r, 0, 180, bg_col);
    } else if (!strcmp(name, "Brightness")) {
        d.fillCircle(cx, cy, r/2, col);
        for (int a = 0; a < 360; a += 45) {
            float rad = a * 3.14159f / 180.0f;
            int x1 = cx + (int)(cosf(rad) * (r/2 + 2));
            int y1 = cy + (int)(sinf(rad) * (r/2 + 2));
            int x2 = cx + (int)(cosf(rad) * r);
            int y2 = cy + (int)(sinf(rad) * r);
            d.drawLine(x1, y1, x2, y2, col);
        }
    } else if (!strcmp(name, "Battery mode")) {
        d.drawRect(cx - r, cy - r/2, r * 2 - 2, r, col);
        d.fillRect(cx + r - 2, cy - r/4, 2, r/2, col);
        d.fillRect(cx - r + 2, cy - r/2 + 2, r, r - 4, col);
    } else if (!strcmp(name, "Boot style")) {
        d.drawRoundRect(cx - r, cy - r/2, r * 2, r * 3 / 2, r/6, col);
        d.drawFastHLine(cx - r + 2, cy - r/4, r * 2 - 4, col);
    } else if (!strcmp(name, "Device name")) {
        d.drawRoundRect(cx - r, cy - r/2, r * 2 - r/3, r, r/4, col);
        d.fillCircle(cx + r - r/3, cy, 2, col);
    } else if (!strcmp(name, "Sound volume")) {
        d.fillRect(cx - r, cy - r/4, r/2, r/2, col);
        d.fillTriangle(cx - r/2, cy - r/4, cx - r/2, cy + r/4, cx, cy + r/2, col);
        d.fillTriangle(cx - r/2, cy - r/4, cx, cy - r/2, cx, cy + r/2, col);
        d.drawArc(cx + r/4, cy, r/3, r/3 + 2, 300, 60, col);
    } else if (!strcmp(name, "WiFi")) {
        d.fillRect(cx - r/2, cy + r/2 - 2, 3, 3, col);
        d.fillRect(cx - r/6, cy + r/4 - 2, 3, r/2, col);
        d.fillRect(cx + r/6, cy - 2, 3, r - 2, col);
    } else if (!strcmp(name, "Reset steps")) {
        d.drawArc(cx, cy, r/2, r, 30, 300, col);
        d.fillTriangle(cx + r - 2, cy - r/3, cx + r + 3, cy - r/3, cx + r, cy - r/3 - 5, col);
    } else if (!strcmp(name, "Check for updates")) {
        d.drawRoundRect(cx - r, cy - r/3, r * 2, r, r/3, col);
        d.drawLine(cx, cy - r, cx, cy - r/4, col);
        d.drawLine(cx - r/3, cy - r/2, cx, cy - r, col);
        d.drawLine(cx + r/3, cy - r/2, cx, cy - r, col);
    } else if (!strcmp(name, "Screen lock") || !strcmp(name, "Lock screen")) {
        d.drawRoundRect(cx - r/2, cy - r/6, r, r * 2 / 3, r/6, col);
        d.drawArc(cx, cy - r/3, r/3, r/3 + 2, 180, 360, col);
    } else if (!strcmp(name, "Status bar")) {
        d.drawRoundRect(cx - r, cy - r/4, r * 2, r/2, r/4, col);
        d.fillCircle(cx - r/2, cy, 2, col);
        d.fillCircle(cx, cy, 2, col);
    } else if (!strcmp(name, "BG gradient")) {
        for (int i = 0; i < 4; i++) {
            uint8_t shade = 60 + i * 50;
            uint32_t c = ((uint32_t)shade << 16) | ((uint32_t)shade << 8) | shade;
            d.fillRect(cx - r + i * (r/2), cy - r/2, r/2, r, c);
        }
    } else if (!strcmp(name, "SSH Server")) {
        d.drawRect(cx - r, cy - r/2, r * 2, r, col);
        d.drawLine(cx - r + 4, cy - r/4, cx - r/2, cy, col);
        d.drawLine(cx - r/2, cy, cx - r + 4, cy + r/4, col);
        d.drawLine(cx - r/3, cy + r/4, cx, cy + r/4, col);
    } else if (!strcmp(name, "Menu UI style")) {
        int s = r / 2;
        d.fillRect(cx - s, cy - s, s - 2, s - 2, col);
        d.fillRect(cx, cy - s, s - 2, s - 2, col);
        d.fillRect(cx - s, cy, s - 2, s - 2, col);
        d.fillRect(cx, cy, s - 2, s - 2, col);
    } else if (!strcmp(name, "CPU freq")) {
        d.drawRect(cx - r/2, cy - r/2, r, r, col);
        for (int i = -1; i <= 1; i++) {
            d.drawLine(cx + i * r/3, cy - r/2, cx + i * r/3, cy - r, col);
            d.drawLine(cx + i * r/3, cy + r/2, cx + i * r/3, cy + r, col);
        }
    } else if (!strcmp(name, "Power")) {
        d.drawCircle(cx, cy, r, col);
        d.drawLine(cx, cy - r, cx, cy - r/3, col);
        d.drawFastHLine(cx - 2, cy - r, 4, bg_col);
    } else if (!strcmp(name, "Font")) {
        d.setTextSize(1);
        d.setCursor(cx - r/2, cy - r/3);
        d.setTextColor(col, bg_col);
        d.print("Aa");
    } else if (!strcmp(name, "Keyboard nav")) {
        d.drawRoundRect(cx - r, cy - r/2, r * 2, r, r/6, col);
        d.drawFastHLine(cx - r + 3, cy - r/6, r * 2 - 6, col);
        d.drawFastHLine(cx - r + 3, cy + r/6, r * 2 - 6, col);
    } else if (!strcmp(name, "Back")) {
        d.drawLine(cx + r/2, cy - r/2, cx - r/2, cy, col);
        d.drawLine(cx - r/2, cy, cx + r/2, cy + r/2, col);
        d.drawLine(cx - r/2, cy, cx + r, cy, col);
    } else {
        d.fillCircle(cx, cy, r/3, col);
    }
}

static void settings_clamp_scroll(int visible_rows) {
    if (s_selected < s_scroll_top) {
        s_scroll_top = s_selected;
    }
    if (s_selected >= s_scroll_top + visible_rows) {
        s_scroll_top = s_selected - visible_rows + 1;
    }
    if (s_scroll_top < 0) s_scroll_top = 0;
    int max_top = K85_SETTINGS_ITEM_COUNT - visible_rows;
    if (max_top < 0) max_top = 0;
    if (s_scroll_top > max_top) s_scroll_top = max_top;
}

static void settings_draw_plain(void) {
    settings_clamp_scroll(K85_SETTINGS_VISIBLE_ROWS);

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(fg, bg);
    M5.Display.print("Settings");

    int y = 20;
    char val[40];
    int last_visible = s_scroll_top + K85_SETTINGS_VISIBLE_ROWS;
    if (last_visible > K85_SETTINGS_ITEM_COUNT) last_visible = K85_SETTINGS_ITEM_COUNT;

    for (int i = s_scroll_top; i < last_visible; i++) {
        bool sel = (i == s_selected);
        M5.Display.setCursor(6, y);
        M5.Display.setTextColor(sel ? accent : fg, bg);
        M5.Display.print(sel ? "> " : "  ");
        M5.Display.print(k85_settings_labels[i]);

        settings_value_str(val, sizeof(val), i);
        if (val[0]) {
            M5.Display.setCursor(140, y);
            M5.Display.print(val);
        }
        y += 14;
    }

    if (K85_SETTINGS_ITEM_COUNT > K85_SETTINGS_VISIBLE_ROWS) {
        if (s_scroll_top > 0) {
            M5.Display.setCursor(230, 20);
            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.print("^");
        }
        if (last_visible < K85_SETTINGS_ITEM_COUNT) {
            M5.Display.setCursor(230, 20 + (K85_SETTINGS_VISIBLE_ROWS - 1) * 14);
            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.print("v");
        }
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(6, y + 6);
    M5.Display.print("A=next B=change/back A+B=exit");
}

// Список с иконками + значение справа (то же, что и обычный список, но с
// иконкой слева от названия - значение сохраняем, так как это важная часть
// Settings, в отличие от Tools/Games).
static void settings_draw_list_icons(void) {
    const int visible_rows = 5; // строка чуть выше, места под иконку меньше
    settings_clamp_scroll(visible_rows);

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(accent, bg);
    M5.Display.print("Settings");

    int y = 20;
    const int line_h = 20;
    char val[40];
    int last_visible = s_scroll_top + visible_rows;
    if (last_visible > K85_SETTINGS_ITEM_COUNT) last_visible = K85_SETTINGS_ITEM_COUNT;

    for (int i = s_scroll_top; i < last_visible; i++) {
        bool sel = (i == s_selected);
        if (sel) {
            M5.Display.fillRoundRect(2, y - 2, M5.Display.width() - 4, 16, 4, accent);
        }
        uint32_t item_fg = sel ? 0x000000 : fg;
        uint32_t item_bg = sel ? accent : bg;
        draw_settings_icon(12, y + 6, 7, k85_settings_labels[i], item_fg, item_bg);

        M5.Display.setTextColor(item_fg, item_bg);
        M5.Display.setCursor(24, y + 2);
        M5.Display.print(k85_settings_labels[i]);

        settings_value_str(val, sizeof(val), i);
        if (val[0]) {
            M5.Display.setCursor(150, y + 2);
            M5.Display.print(val);
        }
        y += line_h;
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    if (s_scroll_top > 0) {
        M5.Display.setCursor(230, 20);
        M5.Display.print("^");
    }
    if (last_visible < K85_SETTINGS_ITEM_COUNT) {
        M5.Display.setCursor(230, 20 + (visible_rows - 1) * line_h);
        M5.Display.print("v");
    }
    M5.Display.setCursor(6, y + 6);
    M5.Display.print("A=next B=change/back A+B=exit");
}

// Сетка иконок, без колонки значений (как в Tools/Games) - страницы по 8
// пунктов (4x2), листаются автоматически при переходе s_selected за пределы
// текущей страницы (та же логика, что в menu.cpp/list_menu.cpp).
static void settings_draw_grid(void) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int w = M5.Display.width();
    int h = M5.Display.height();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(accent, bg);
    M5.Display.print("Settings");

    const int cols = 4;
    const int rows = 2;
    const int per_page = cols * rows;
    const int start_y = 16;
    int grid_h = h - start_y - 10;
    int cell_w = w / cols;
    int cell_h = grid_h / rows;

    int page = s_selected / per_page;
    int page_count = (K85_SETTINGS_ITEM_COUNT + per_page - 1) / per_page;
    int page_start = page * per_page;
    int page_end = page_start + per_page;
    if (page_end > K85_SETTINGS_ITEM_COUNT) page_end = K85_SETTINGS_ITEM_COUNT;

    for (int i = page_start; i < page_end; i++) {
        int local = i - page_start;
        int col = local % cols;
        int row = local / cols;
        int cx = col * cell_w + cell_w / 2;
        int cy = start_y + row * cell_h + cell_h / 2 - 6;

        bool sel = (i == s_selected);
        if (sel) {
            M5.Display.fillRoundRect(col * cell_w + 3, start_y + row * cell_h + 2,
                                      cell_w - 6, cell_h - 4, 6, accent);
        }
        uint32_t item_fg = sel ? 0x000000 : fg;
        uint32_t item_bg = sel ? accent : bg;
        draw_settings_icon(cx, cy, 12, k85_settings_labels[i], item_fg, item_bg);

        M5.Display.setTextSize(1);
        M5.Display.setTextColor(item_fg, item_bg);
        char short_label[16];
        int max_chars = (cell_w - 4) / 6;
        if (max_chars > 15) max_chars = 15;
        if (max_chars < 1) max_chars = 1;
        snprintf(short_label, sizeof(short_label), "%.*s", max_chars, k85_settings_labels[i]);
        int tx = col * cell_w + (cell_w - (int)strlen(short_label) * 6) / 2;
        if (tx < col * cell_w) tx = col * cell_w + 1;
        M5.Display.setCursor(tx, start_y + row * cell_h + cell_h - 12);
        M5.Display.print(short_label);
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    if (page_count > 1) {
        char pg[32];
        snprintf(pg, sizeof(pg), "%d/%d", page + 1, page_count);
        M5.Display.setCursor(w - (int)strlen(pg) * 6 - 4, h - 10);
        M5.Display.print(pg);
    }
}

static void settings_draw(void) {
    int style = g_config.menu_ui_style;
    switch (style) {
        case 1: settings_draw_grid(); break;
        case 2: settings_draw_list_icons(); break;
        default: settings_draw_plain(); break;
    }
}

static void settings_apply_item(int idx) {
    switch (idx) {
        case 0:
            g_config.theme_idx = (g_config.theme_idx + 1) % k85_theme_count();
            break;
        case 1:
            g_config.brightness_active += 10;
            if (g_config.brightness_active > 100) g_config.brightness_active = 10;
            M5.Display.setBrightness(g_config.brightness_active);
            break;
        case 2:
            g_config.battery_mode_idx = (g_config.battery_mode_idx + 1) % K85_BATTERY_MODE_COUNT;
            break;
        case 3:
            g_config.bootstyle_idx = (g_config.bootstyle_idx + 1) % K85_BOOT_STYLE_COUNT;
            break;
        case 4:
            g_config.device_name_idx = (g_config.device_name_idx + 1) % K85_DEVICE_NAME_COUNT;
            break;
        case 5: {
            int v = k85_get_sound_volume() + 10;
            if (v > 100) v = 0;
            k85_set_sound_volume(v);
            k85_apply_sound_volume();
            break;
        }
        case 6:
            if (g_config.wifi_disabled) {
                k85_show_message("WiFi module disabled\n(k85os-menu)");
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            }
            if (g_config.wifi_saved) {
                k85_show_message("Connecting WiFi...");
                bool ok = k85_wifi_connect_saved();
                k85_show_message(ok ? "WiFi connected" : "WiFi failed");
                vTaskDelay(pdMS_TO_TICKS(800));
            } else {
                k85_show_message("No saved WiFi");
                vTaskDelay(pdMS_TO_TICKS(800));
            }
            break;
        case 7:
            k85_reset_step_counter();
            break;
        case 8: {
            if (!k85_wifi_is_connected()) {
                k85_show_message("Connect WiFi first\nA+B=back");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                break;
            }
            char ver[16];
            char url[256];
            char sig_url[256];
            k85_show_message("Checking...");
            if (!k85_ota_check_update(ver, sizeof(ver), url, sizeof(url), sig_url, sizeof(sig_url))) {
                k85_show_message("No update found\nA+B=back");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                break;
            }

            char confirm_msg[64];
            snprintf(confirm_msg, sizeof(confirm_msg), "Update v%s found\nB=install A+B=cancel", ver);
            k85_show_message(confirm_msg);

            bool do_install = false;
            while (true) {
                k85_input_update();
                if (k85_ab_held(500)) { k85_wait_ab_release(); do_install = false; break; }
                if (k85_btn_b_pressed()) { do_install = true; break; }
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            if (!do_install) break;

            k85_show_message("Installing 0%...");
            static char progress_msg[32];
            bool ok = k85_fwflash_from_url(url, sig_url, [](int percent) {
                snprintf(progress_msg, sizeof(progress_msg), "Installing %d%%...", percent);
                k85_show_message(progress_msg);
            });
            if (ok) {
                k85_show_message("Verified & written!\nActivate via GRUB ->\nAlt Firmware\nA+B=back");
            } else {
                k85_show_message("Update failed\n(bad signature or\nnetwork error)\nA+B=back");
            }
            while (true) {
                k85_input_update();
                if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            break;
        }
        case 9: {
            if (g_config.lock_enabled) {
                k85_show_message("Disable screen lock?\nB=confirm A+B=cancel");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); goto lock_done; }
                    if (k85_btn_b_pressed()) break;
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                g_config.lock_enabled = false;
                g_config.lock_password[0] = 0;
                k85_show_message("Screen lock disabled\nA+B=back");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            } else {
                char pass[32] = "";
                if (k85_text_input("Set lock password:", "", pass, sizeof(pass)) && pass[0]) {
                    k85_lock_hash_password(pass, g_config.lock_password, sizeof(g_config.lock_password));
                    g_config.lock_enabled = true;
                    k85_show_message("Screen lock enabled\nA+B=back");
                    while (true) {
                        k85_input_update();
                        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                        vTaskDelay(pdMS_TO_TICKS(30));
                    }
                }
            }
            lock_done:
            break;
        }
        case 10:
            k85_run_status_bar_settings();
            break;
        case 11:
            g_config.bg_gradient_enabled = !g_config.bg_gradient_enabled;
            break;
        case 12:
            k85_run_lock_screen_settings();
            break;
        case 13: {
            if (g_config.ssh_enabled) {
                k85_show_message("Disable SSH server?\nB=confirm A+B=cancel");
                bool confirmed = false;
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); goto ssh_done; }
                    if (k85_btn_b_pressed()) { confirmed = true; break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                if (confirmed) {
                    k85_ssh_server_stop();
                    g_config.ssh_enabled = false;
                    k85_show_message("SSH server disabled\nA+B=back");
                    while (true) {
                        k85_input_update();
                        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                        vTaskDelay(pdMS_TO_TICKS(30));
                    }
                }
            } else {
                char user[32] = "";
                if (!k85_text_input("SSH username:", g_config.ssh_username, user, sizeof(user)) || !user[0]) break;

                char pass[64] = "";
                if (!k85_text_input("SSH password:", "", pass, sizeof(pass)) || !pass[0]) break;

                snprintf(g_config.ssh_username, sizeof(g_config.ssh_username), "%s", user);
                k85_ssh_hash_password(pass, g_config.ssh_password_hash);
                g_config.ssh_enabled = true;
                k85_config_save();

                ESP_LOGI("k85_settings", "internal free: %u, largest block: %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                K85SshStartResult ssh_res = k85_ssh_server_start();
                if (ssh_res == K85_SSH_START_OK) {
                    k85_show_message("SSH server started\nA+B=back");
                } else if (ssh_res == K85_SSH_START_NO_WIFI) {
                    k85_show_message("SSH start failed\n(WiFi not saved)\nA+B=back");
                } else if (ssh_res == K85_SSH_START_ALREADY_RUNNING) {
                    k85_show_message("SSH already running\n(or stuck - reboot)\nA+B=back");
                } else {
                    k85_show_message("SSH start failed\n(task create error)\nA+B=back");
                }
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }
            ssh_done:
            break;
        }
        case 14:
            g_config.menu_ui_style = (g_config.menu_ui_style + 1) % 3;
            break;
        case 15: {
            int new_mhz = k85_cpu_freq_cycle();
            if (new_mhz == 80) {
                k85_show_message("Warning: 80MHz may\ncause WiFi/BT issues\nA+B=back");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }
            break;
        }
        case 16:
            k85_run_power_menu();
            break;
        case 17:
            g_config.font_idx = (g_config.font_idx + 1) % K85_FONT_COUNT;
            k85_apply_font(g_config.font_idx);
            break;
        case 18:
            g_config.kbd_nav_mode = (g_config.kbd_nav_mode == 0) ? 1 : 0;
            break;
        default:
            break;
    }
    k85_config_save();
}

void k85_run_settings_menu(void) {
    s_selected = 0;
    s_scroll_top = 0;
    settings_draw();
    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            k85_config_save();
            return;
        }

        if (k85_btn_a_pressed()) {
            s_selected = (s_selected + 1) % K85_SETTINGS_ITEM_COUNT;
            settings_draw();
        }

        if (k85_btn_b_pressed()) {
            if (s_selected == K85_SETTINGS_BACK_IDX) {
                k85_config_save();
                return;
            }
            settings_apply_item(s_selected);
            settings_draw();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
