#include "terminal.h"
#include "config.h"
#include "theme.h"
#include "input.h"
#include "power.h"
#include "battery.h"
#include "common.h"
#include "rtc_ntp.h"
#include "wifi.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#include <cstdio>
#include "text_input.h"
#include "list_menu.h"
#include "core/shell_commands.h"

#define K85_TERMINAL_ITEM_COUNT 6

static const char *k85_terminal_labels[K85_TERMINAL_ITEM_COUNT] = {
    "Free RAM", "Uptime", "WiFi status", "Command line", "Reboot", "Exit",
};

static int s_selected = 0;

static void terminal_draw(void) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, 4);
    M5.Display.setTextColor(fg, bg);
    M5.Display.print("Terminal");

    int y = 20;
    for (int i = 0; i < K85_TERMINAL_ITEM_COUNT; i++) {
        bool sel = (i == s_selected);
        M5.Display.setCursor(6, y);
        M5.Display.setTextColor(sel ? accent : fg, bg);
        M5.Display.print(sel ? "> " : "  ");
        M5.Display.print(k85_terminal_labels[i]);
        y += 14;
    }

    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(6, y + 6);
    M5.Display.print("A=down B=enter");
}

static void terminal_show_result(const char *text) {
    k85_show_message(text);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// Прокручиваемый постраничный вывод с переносом длинных строк по словам -
// в отличие от общего k85_area_show (жёсткий лимит 10 строк без переноса),
// тут строки, не влезающие по ширине экрана, переносятся на следующую,
// и весь вывод можно листать кнопкой A, а не только видеть первые 10 строк.
static void terminal_show_output_scrollable(const char *raw_text) {
    #define K85_TERM_MAX_WRAPPED 48
    static char wrapped_storage[2048];
    const char *wrapped_lines[K85_TERM_MAX_WRAPPED];
    int wrapped_count = 0;

    int W = M5.Display.width();
    int max_chars = (W - 8) / 6;
    if (max_chars < 8) max_chars = 8;

    size_t storage_used = 0;
    char raw_copy[512];
    snprintf(raw_copy, sizeof(raw_copy), "%s", raw_text);

    char *line_start = raw_copy;
    while (*line_start && wrapped_count < K85_TERM_MAX_WRAPPED) {
        char *nl = strpbrk(line_start, "\r\n");
        char saved = 0;
        if (nl) { saved = *nl; *nl = 0; }

        // разбиваем эту логическую строку на куски по max_chars символов
        size_t len = strlen(line_start);
        size_t off = 0;
        do {
            size_t chunk = len - off;
            if (chunk > (size_t)max_chars) chunk = max_chars;
            if (storage_used + chunk + 1 >= sizeof(wrapped_storage)) break;
            char *dst = wrapped_storage + storage_used;
            memcpy(dst, line_start + off, chunk);
            dst[chunk] = 0;
            wrapped_lines[wrapped_count++] = dst;
            storage_used += chunk + 1;
            off += chunk;
        } while (off < len && wrapped_count < K85_TERM_MAX_WRAPPED);
        if (len == 0 && wrapped_count < K85_TERM_MAX_WRAPPED) {
            wrapped_storage[storage_used] = 0;
            wrapped_lines[wrapped_count++] = wrapped_storage + storage_used;
            storage_used += 1;
        }

        if (!nl) break;
        *nl = saved;
        line_start = nl;
        while (*line_start == '\r' || *line_start == '\n') line_start++;
    }

    if (wrapped_count == 0) {
        wrapped_storage[0] = 0;
        wrapped_lines[0] = wrapped_storage;
        wrapped_count = 1;
    }

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int H = M5.Display.height();
    const int line_h = 13; // с запасом, не жмётся вплотную - не наезжает
    const int top = 16;
    int visible_lines = (H - top - 12) / line_h;
    if (visible_lines < 1) visible_lines = 1;

    int scroll = 0;

    auto redraw = [&]() {
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.print("$ output");

        int y = top;
        for (int i = scroll; i < scroll + visible_lines && i < wrapped_count; i++) {
            M5.Display.setTextColor(fg, bg);
            M5.Display.setCursor(4, y);
            M5.Display.print(wrapped_lines[i]);
            y += line_h;
        }

        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, H - 12);
        if (wrapped_count > visible_lines) {
            M5.Display.print("A=scroll A+B=back");
        } else {
            M5.Display.print("A+B=back");
        }
        k85_draw_battery_icon();
    };

    redraw();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed()) {
            if (wrapped_count > visible_lines) {
                scroll += visible_lines;
                if (scroll >= wrapped_count) scroll = 0;
                redraw();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// Реальная командная строка - ввод текста через k85_text_input,
// выполнение через общий k85_shell_run_command (тот же код, что и SSH).
static void terminal_run_shell(void) {
    while (true) {
        char cmd[64] = "";
        if (!k85_text_input("$ (Exit=back)", "", cmd, sizeof(cmd))) return;

        char resp[256];
        k85_shell_run_command(cmd, resp, sizeof(resp));

        if (!strcmp(cmd, "exit")) return;

        if (resp[0] == 0) {
            terminal_show_output_scrollable("(no output)");
        } else {
            terminal_show_output_scrollable(resp);
        }

        bool reboot = !strcmp(cmd, "reboot");
        if (reboot) {
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}
static void terminal_run_command(int idx, bool *exit_to_main) {
    char buf[128];
    switch (idx) {
        case 0: { // Free RAM
            uint32_t free_kb = esp_get_free_heap_size() / 1024;
            snprintf(buf, sizeof(buf), "Free RAM: %lu KB\nA+B=back", (unsigned long)free_kb);
            terminal_show_result(buf);
            break;
        }
        case 1: { // Uptime
            snprintf(buf, sizeof(buf), "Uptime: %s\nA+B=back", k85_get_uptime_str());
            terminal_show_result(buf);
            break;
        }
        case 2: { // WiFi status
            if (k85_wifi_is_connected()) {
                snprintf(buf, sizeof(buf), "WiFi: %.31s\n(connected)\nA+B=back", g_config.wifi_ssid);
            } else {
                snprintf(buf, sizeof(buf), "WiFi: disconnected\nA+B=back");
            }
            terminal_show_result(buf);
            break;
        }
        case 3: { // Command line
            terminal_run_shell();
            break;
        }
        case 4: { // Reboot
            k85_show_message("Rebooting...");
            vTaskDelay(pdMS_TO_TICKS(800));
            esp_restart();
            break;
        }
        case 5: { // Exit - сразу в главное меню
            *exit_to_main = true;
            break;
        }
        default:
            break;
    }
}

bool k85_run_terminal(void) {
    s_selected = 0;
    terminal_draw();
    while (true) {
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return false; // обычный выход - назад в Tools
        }

        if (k85_btn_a_pressed()) {
            s_selected = (s_selected + 1) % K85_TERMINAL_ITEM_COUNT;
            terminal_draw();
        }

        if (k85_btn_b_pressed()) {
            bool exit_to_main = false;
            terminal_run_command(s_selected, &exit_to_main);
            if (exit_to_main) return true;
            terminal_draw();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

