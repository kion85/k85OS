#include "terminal.h"
#include "config.h"
#include "theme.h"
#include "input.h"
#include "power.h"
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

// Реальная командная строка — ввод текста через k85_text_input,
// выполнение через общий k85_shell_run_command (тот же код, что и SSH).
static void terminal_run_shell(void) {
    while (true) {
        char cmd[64] = "";
        if (!k85_text_input("$ (Exit=back)", "", cmd, sizeof(cmd))) return;

        char resp[256];
        k85_shell_run_command(cmd, resp, sizeof(resp));

        if (!strcmp(cmd, "exit")) return;

        const char *lines[10];
        int count = 0;
        char *p = resp;
        while (*p && count < 10) {
            lines[count++] = p;
            char *nl = strpbrk(p, "\r\n");
            if (!nl) break;
            while (*nl == '\r' || *nl == '\n') { *nl = 0; nl++; }
            p = nl;
        }
        if (count == 0) {
            const char *empty[] = { "(no output)" };
            k85_area_show(empty, 1, "$");
        } else {
            k85_area_show(lines, count, "$");
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

