#pragma once
#include <stddef.h>

#define K85_MAX_TOTP_ENTRIES 5

typedef struct {
    char name[24];
    char secret_base32[64]; // RFC4648 base32, без padding ('=') - как обычно даёт сайт
} k85_totp_entry_t;

// Добавляет новый аккаунт (валидирует, что secret_base32 декодируется хоть
// в один байт). Возвращает false если список полон или секрет невалиден.
bool k85_totp_add(const char *name, const char *secret_base32);
bool k85_totp_remove(int idx);
int  k85_totp_count(void);
const char *k85_totp_name(int idx);

// Текущий 6-значный код для аккаунта idx (записывает "000000"-"999999" + '\0').
// Возвращает false при невалидном idx или ошибке декодирования/HMAC.
bool k85_totp_generate_code(int idx, char *out_code, size_t out_size);

// Сколько секунд осталось до смены текущего кода (0..29).
int k85_totp_seconds_remaining(void);
