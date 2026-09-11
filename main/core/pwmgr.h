#pragma once
#include <stddef.h>
#include <stdint.h>

#define K85_MAX_PW_ENTRIES 10
#define K85_PWMGR_VERIFIER_HEX_LEN 96 // hex(salt 16) + hex(verifier 32) = 96 символов + '\0'

typedef struct {
    char name[24];
    char username[64];
    uint8_t iv[16];
    uint8_t ciphertext[80]; // PKCS7-padded AES-256-CBC, максимум пароль 64 байта -> padded до 80
    int ciphertext_len;
} k85_pwentry_t;

// true, если мастер-пароль уже задан (g_config.pwmgr_master_verifier не пуст).
bool k85_pwmgr_is_setup(void);

// Первоначальная установка мастер-пароля. Сразу разблокирует сессию (ключ
// в RAM), сохраняет verifier в конфиг.
bool k85_pwmgr_setup_master(const char *master_password);

// Проверяет мастер-пароль; при успехе кладёт AES-ключ в RAM на текущую
// сессию (до k85_pwmgr_lock() или перезапуска устройства) и возвращает true.
bool k85_pwmgr_unlock(const char *master_password);

// Стирает ключ из RAM. Обязательно вызывать при выходе из инструмента -
// иначе ключ остаётся в памяти пока устройство не перезагрузится.
void k85_pwmgr_lock(void);
bool k85_pwmgr_is_unlocked(void);

int k85_pwmgr_count(void);
const char *k85_pwmgr_name(int idx);
const char *k85_pwmgr_username(int idx);

// Требует разблокированной сессии (k85_pwmgr_is_unlocked() == true).
bool k85_pwmgr_get_password(int idx, char *out, size_t out_size);
bool k85_pwmgr_add(const char *name, const char *username, const char *password);
bool k85_pwmgr_remove(int idx);
