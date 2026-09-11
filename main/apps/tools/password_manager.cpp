#include "password_manager.h"
#include "pwmgr.h"
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

// Первый запуск - мастер-пароль ещё не задан. Требуем ввод дважды, чтобы
// не отрезать себя от всех паролей опечаткой.
static bool run_first_time_setup(void) {
    char pass1[64] = "";
    if (!k85_text_input("Set master password:", "", pass1, sizeof(pass1)) || strlen(pass1) < 4) {
        k85_show_message("Too short (min 4)\nA+B=back");
        wait_ab_exit();
        return false;
    }
    char pass2[64] = "";
    if (!k85_text_input("Confirm master password:", "", pass2, sizeof(pass2))) return false;

    if (strcmp(pass1, pass2) != 0) {
        k85_show_message("Passwords don't match\nA+B=back");
        wait_ab_exit();
        return false;
    }

    if (!k85_pwmgr_setup_master(pass1)) {
        k85_show_message("Setup failed\nA+B=back");
        wait_ab_exit();
        return false;
    }
    return true;
}

static bool run_unlock_prompt(void) {
    char pass[64] = "";
    if (!k85_text_input("Master password:", "", pass, sizeof(pass))) return false;
    if (!k85_pwmgr_unlock(pass)) {
        k85_show_message("Wrong password\nA+B=back");
        wait_ab_exit();
        return false;
    }
    return true;
}

static void show_entry(int idx) {
    char password[80] = "";
    bool ok = k85_pwmgr_get_password(idx, password, sizeof(password));

    char msg[220];
    if (ok) {
        snprintf(msg, sizeof(msg), "%s\nUser: %s\nPass: %s\nA+B=back",
                 k85_pwmgr_name(idx), k85_pwmgr_username(idx), password);
    } else {
        snprintf(msg, sizeof(msg), "%s\nDecrypt failed\nA+B=back", k85_pwmgr_name(idx));
    }
    k85_show_message(msg);
    wait_ab_exit();

    // Затираем расшифрованный пароль из локальной переменной сразу после
    // показа - не оставляем его валяться в стеке дольше необходимого.
    memset(password, 0, sizeof(password));
}

static void prompt_new_entry(void) {
    char name[24] = "";
    if (!k85_text_input("Site/service name:", "", name, sizeof(name)) || !name[0]) return;

    char username[64] = "";
    if (!k85_text_input("Username/email:", "", username, sizeof(username))) return;

    char password[64] = "";
    if (!k85_text_input("Password:", "", password, sizeof(password)) || !password[0]) return;

    bool ok = k85_pwmgr_add(name, username, password);
    memset(password, 0, sizeof(password));

    if (!ok) {
        k85_show_message("Add failed\n(list full, max 10)\nA+B=back");
        wait_ab_exit();
    }
}

void k85_run_password_manager(void) {
    if (!k85_pwmgr_is_setup()) {
        if (!run_first_time_setup()) return;
    } else {
        if (!run_unlock_prompt()) return;
    }

    // С этой точки сессия разблокирована - обязательно вызываем
    // k85_pwmgr_lock() на любом пути выхода из функции, иначе AES-ключ
    // остаётся в RAM до перезагрузки устройства.
    while (true) {
        int n = k85_pwmgr_count();

        const char *items[K85_MAX_PW_ENTRIES + 2];
        for (int i = 0; i < n; i++) items[i] = k85_pwmgr_name(i);
        items[n] = "+ Add Entry";
        items[n + 1] = "Back";

        int idx = k85_run_list_menu("PASSWORD MANAGER", items, n + 2, nullptr);
        if (idx < 0 || idx == n + 1) {
            k85_pwmgr_lock();
            return;
        }

        if (idx == n) {
            prompt_new_entry();
        } else {
            show_entry(idx);
        }
    }
}
