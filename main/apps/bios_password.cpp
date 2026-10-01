#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "bios_password.h"
#include "common.h"
#include "input.h"
#include "list_menu.h"
#include "text_input.h"
#include "../core/lock_auth.h"
#include "../core/svc_cfg.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

// Хеш пароля лежит в k85_svc.cfg (ключ bios_pw): PBKDF2-HMAC-SHA256, соль 16 байт,
// тот же формат, что у пароля экрана блокировки. Файл закрыт для shell и FTP.
#define K85_BIOS_PW_KEY "bios_pw"
#define K85_BIOS_PW_MIN 4

static int s_fail_count = 0;

static void wipe(char *buf, size_t n) {
    volatile char *p = buf;
    while (n--) *p++ = 0;
}

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void show_and_wait(const char *msg) {
    k85_show_message(msg);
    wait_ab_exit();
}

static bool has_password(char *stored, size_t n) {
    return k85_svc_get_str(K85_BIOS_PW_KEY, stored, n);
}

// Спрашивает пароль и сверяет с сохранённым. false = отмена или неверный пароль.
static bool ask_and_verify(const char *prompt, const char *stored) {
    char pw[66];
    if (!k85_text_input(prompt, "", pw, sizeof(pw))) { wipe(pw, sizeof(pw)); return false; }
    bool ok = k85_lock_verify_password(pw, stored);
    wipe(pw, sizeof(pw));
    return ok;
}

bool k85_bios_password_gate(void) {
    char stored[104];
    if (!has_password(stored, sizeof(stored))) return true;

    while (true) {
        char pw[66];
        if (!k85_text_input("BIOS password", "", pw, sizeof(pw))) { wipe(pw, sizeof(pw)); return false; }
        bool ok = k85_lock_verify_password(pw, stored);
        wipe(pw, sizeof(pw));
        if (ok) { s_fail_count = 0; return true; }

        s_fail_count++;
        int wait_s = s_fail_count * 2;
        if (wait_s > 20) wait_s = 20;
        char msg[48];
        snprintf(msg, sizeof(msg), "Wrong password\nwait %d s", wait_s);
        k85_show_message(msg);
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
        if (s_fail_count % 5 == 0) return false;   // 5 неудач подряд - выходим из BIOS
    }
}

static void set_password(bool has, const char *stored) {
    if (has && !ask_and_verify("Current password", stored)) {
        show_and_wait("Wrong password\nor cancelled\nA+B=back");
        return;
    }
    char a[66], b[66];
    if (!k85_text_input("New password (4+)", "", a, sizeof(a))) { wipe(a, sizeof(a)); return; }
    if (strlen(a) < K85_BIOS_PW_MIN) {
        wipe(a, sizeof(a));
        show_and_wait("Too short (min 4)\nA+B=back");
        return;
    }
    if (!k85_text_input("Repeat password", "", b, sizeof(b))) { wipe(a, sizeof(a)); wipe(b, sizeof(b)); return; }
    bool same = (strcmp(a, b) == 0);
    wipe(b, sizeof(b));
    if (!same) {
        wipe(a, sizeof(a));
        show_and_wait("Passwords differ\nA+B=back");
        return;
    }
    char hex[104];
    k85_lock_hash_password(a, hex, sizeof(hex));
    wipe(a, sizeof(a));
    k85_svc_set_str(K85_BIOS_PW_KEY, hex);
    show_and_wait("BIOS password set\nA+B=back");
}

static void remove_password(bool has, const char *stored) {
    if (!has) { show_and_wait("No password set\nA+B=back"); return; }
    if (!ask_and_verify("Current password", stored)) {
        show_and_wait("Wrong password\nor cancelled\nA+B=back");
        return;
    }
    k85_svc_set_str(K85_BIOS_PW_KEY, nullptr);
    show_and_wait("BIOS password\nremoved\nA+B=back");
}

void k85_run_bios_password_menu(void) {
    while (true) {
        char stored[104];
        bool has = has_password(stored, sizeof(stored));
        char header[40];
        snprintf(header, sizeof(header), "BIOS PASSWORD (%s)", has ? "ON" : "OFF");
        const char *items[] = { has ? "Change password" : "Set password", "Remove password", "Back" };
        int idx = k85_run_list_menu(header, items, 3, nullptr);
        if (idx < 0 || idx == 2) return;
        if (idx == 0) set_password(has, stored);
        else if (idx == 1) remove_password(has, stored);
    }
}

#pragma GCC diagnostic pop
