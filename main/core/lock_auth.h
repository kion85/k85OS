#pragma once
#include <stddef.h>

// PBKDF2-HMAC-SHA256, 10000 итераций, случайная соль 16 байт.
// Формат хранения: hex(соль 16 байт) + hex(хеш 32 байта) = 96 hex-символов + '\0'.

// Генерирует новую соль, хеширует пароль, записывает результат в out (нужно >= 97 байт).
void k85_lock_hash_password(const char *password, char *out, size_t out_size);

// Проверяет введённый пароль против сохранённого hex-значения (соль+хеш).
bool k85_lock_verify_password(const char *password, const char *stored_hex);
