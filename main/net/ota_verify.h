#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Проверяет Ed25519-подпись по SHA-256 хешу данных.
// signature - 64 байта подписи, data/data_len - подписанные данные.
bool k85_ota_verify_signature(const uint8_t *data, size_t data_len, const uint8_t *signature);

// То же самое, но подпись передана как hex-строка (128 символов = 64 байта).
bool k85_ota_verify_signature_hex(const uint8_t *data, size_t data_len, const char *signature_hex);