#include "totp_auth.h"
#include "totp.h"
#include "common.h"
#include "theme.h"
#include "input.h"
#include "list_menu.h"
#include "text_input.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// Экран живого кода: обновляется каждую секунду, показывает прогресс-бар
// до смены кода (30-секундное окно TOTP), A+B выходит обратно к списку.
static void run_code_screen(int idx) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int w = M5.Display.width();
    int h = M5.Display.height();

    char code[8] = "------";
    int last_remaining = -1;

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }

        int remaining = k85_totp_seconds_remaining();
        if (remaining != last_remaining) {
            last_remaining = remaining;
            if (!k85_totp_generate_code(idx, code, sizeof(code))) {
                snprintf(code, sizeof(code), "ERROR");
            }

            M5.Display.fillScreen(bg);
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(accent, bg);
            M5.Display.setCursor(4, 4);
            M5.Display.print(k85_totp_name(idx));

            // Крупный код по центру, с пробелом посередине для читаемости
            // (как в настоящих authenticator-приложениях: "123 456").
            char spaced[8];
            if (strlen(code) == 6) {
                snprintf(spaced, sizeof(spaced), "%c%c%c %c%c%c",
                         code[0], code[1], code[2], code[3], code[4], code[5]);
            } else {
                snprintf(spaced, sizeof(spaced), "%s", code);
            }
            M5.Display.setTextSize(3);
            M5.Display.setTextColor(fg, bg);
            int approx_w = (int)strlen(spaced) * 18;
            M5.Display.setCursor((w - approx_w) / 2, h / 2 - 16);
            M5.Display.print(spaced);

            // Прогресс-бар оставшегося времени
            int bar_w = w - 20;
            int fill_w = (int)((float)remaining / 30.0f * bar_w);
            M5.Display.drawRect(10, h / 2 + 20, bar_w, 8, fg);
            M5.Display.fillRect(10, h / 2 + 20, fill_w, 8, remaining <= 5 ? 0xFF0000 : accent);

            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.setCursor(4, h - 12);
            M5.Display.print("A+B=back");
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static bool prompt_new_account(void) {
    char name[24] = "";
    if (!k85_text_input("Account name:", "", name, sizeof(name)) || !name[0]) return false;

    char secret[64] = "";
    if (!k85_text_input("Secret (base32):", "", secret, sizeof(secret)) || !secret[0]) return false;

    if (!k85_totp_add(name, secret)) {
        k85_show_message("Invalid secret or\nlist full (max 5)\nA+B=back");
        wait_ab_exit();
        return false;
    }
    return true;
}

void k85_run_totp_auth(void) {
    while (true) {
        int n = k85_totp_count();

        const char *items[K85_MAX_TOTP_ENTRIES + 2];
        for (int i = 0; i < n; i++) items[i] = k85_totp_name(i);
        items[n] = "+ Add Account";
        items[n + 1] = "Back";

        int idx = k85_run_list_menu("AUTHENTICATOR", items, n + 2, nullptr);
        if (idx < 0 || idx == n + 1) return;

        if (idx == n) {
            prompt_new_account();
        } else {
            run_code_screen(idx);
        }
    }
}