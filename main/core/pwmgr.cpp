#include "pwmgr.h"
#include "config.h"
#include "log.h"

#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/aes.h"
#include "esp_random.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#define PWMGR_ITERATIONS 10000
#define PWMGR_SALT_BYTES 16
#define PWMGR_VERIFIER_BYTES 32
#define PWMGR_KEY_BYTES 32
#define PWMGR_DERIVE_BYTES (PWMGR_VERIFIER_BYTES + PWMGR_KEY_BYTES) // 64

static bool s_unlocked = false;
static unsigned char s_session_key[PWMGR_KEY_BYTES];

static void bytes_to_hex(const unsigned char *data, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) snprintf(out + i * 2, 3, "%02x", data[i]);
}

static bool hex_to_bytes(const char *hex, size_t hex_len, unsigned char *out, size_t out_max) {
    if (hex_len % 2 != 0 || hex_len / 2 > out_max) return false;
    for (size_t i = 0; i < hex_len / 2; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        out[i] = (unsigned char)strtoul(b, nullptr, 16);
    }
    return true;
}

// 64 байта на выходе: [0..32) = verifier (проверка правильности пароля),
// [32..64) = AES-256 ключ. Один PBKDF2-проход вместо двух (дороговат -
// 10000 итераций, лучше не гонять его два раза подряд на каждый unlock).
static void derive(const char *password, const unsigned char *salt, unsigned char *out64) {
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                  (const unsigned char *)password, strlen(password),
                                  salt, PWMGR_SALT_BYTES, PWMGR_ITERATIONS,
                                  PWMGR_DERIVE_BYTES, out64);
}

bool k85_pwmgr_is_setup(void) {
    return g_config.pwmgr_master_verifier[0] != 0;
}

bool k85_pwmgr_setup_master(const char *master_password) {
    unsigned char salt[PWMGR_SALT_BYTES];
    esp_fill_random(salt, sizeof(salt));

    unsigned char derived[PWMGR_DERIVE_BYTES];
    derive(master_password, salt, derived);

    if (sizeof(g_config.pwmgr_master_verifier) < (PWMGR_SALT_BYTES + PWMGR_VERIFIER_BYTES) * 2 + 1) {
        return false; // не должно случиться, буфер в конфиге размечен под этот размер
    }
    bytes_to_hex(salt, PWMGR_SALT_BYTES, g_config.pwmgr_master_verifier);
    bytes_to_hex(derived, PWMGR_VERIFIER_BYTES, g_config.pwmgr_master_verifier + PWMGR_SALT_BYTES * 2);

    memcpy(s_session_key, derived + PWMGR_VERIFIER_BYTES, PWMGR_KEY_BYTES);
    s_unlocked = true;

    k85_config_save();
    return true;
}

bool k85_pwmgr_unlock(const char *master_password) {
    const char *stored = g_config.pwmgr_master_verifier;
    size_t len = strlen(stored);
    if (len != (PWMGR_SALT_BYTES + PWMGR_VERIFIER_BYTES) * 2) return false;

    unsigned char salt[PWMGR_SALT_BYTES];
    unsigned char stored_verifier[PWMGR_VERIFIER_BYTES];
    if (!hex_to_bytes(stored, PWMGR_SALT_BYTES * 2, salt, sizeof(salt))) return false;
    if (!hex_to_bytes(stored + PWMGR_SALT_BYTES * 2, PWMGR_VERIFIER_BYTES * 2, stored_verifier, sizeof(stored_verifier))) return false;

    unsigned char derived[PWMGR_DERIVE_BYTES];
    derive(master_password, salt, derived);

    if (memcmp(derived, stored_verifier, PWMGR_VERIFIER_BYTES) != 0) {
        k85_log("pwmgr: unlock failed (wrong master password)");
        return false;
    }

    memcpy(s_session_key, derived + PWMGR_VERIFIER_BYTES, PWMGR_KEY_BYTES);
    s_unlocked = true;
    return true;
}

void k85_pwmgr_lock(void) {
    memset(s_session_key, 0, sizeof(s_session_key));
    s_unlocked = false;
}

bool k85_pwmgr_is_unlocked(void) { return s_unlocked; }

int k85_pwmgr_count(void) { return g_config.pw_entries_count; }

const char *k85_pwmgr_name(int idx) {
    if (idx < 0 || idx >= g_config.pw_entries_count) return "";
    return g_config.pw_entries[idx].name;
}

const char *k85_pwmgr_username(int idx) {
    if (idx < 0 || idx >= g_config.pw_entries_count) return "";
    return g_config.pw_entries[idx].username;
}

static bool aes_encrypt(const unsigned char *key, const unsigned char *iv_in,
                         const uint8_t *plain, size_t plain_len,
                         uint8_t *out, size_t out_max, int *out_len) {
    size_t pad = 16 - (plain_len % 16);
    size_t total = plain_len + pad;
    if (total > out_max || total > 96) return false;

    uint8_t buf[96];
    memcpy(buf, plain, plain_len);
    for (size_t i = plain_len; i < total; i++) buf[i] = (uint8_t)pad;

    unsigned char iv[16];
    memcpy(iv, iv_in, 16); // mbedtls_aes_crypt_cbc модифицирует iv на месте - работаем с копией

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, 256) != 0) { mbedtls_aes_free(&ctx); return false; }
    bool ok = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, total, iv, buf, out) == 0;
    mbedtls_aes_free(&ctx);

    if (ok) *out_len = (int)total;
    return ok;
}

static bool aes_decrypt(const unsigned char *key, const unsigned char *iv_in,
                         const uint8_t *cipher, int cipher_len,
                         char *out, size_t out_max) {
    if (cipher_len <= 0 || cipher_len % 16 != 0 || cipher_len > 96) return false;

    uint8_t buf[96];
    unsigned char iv[16];
    memcpy(iv, iv_in, 16);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_dec(&ctx, key, 256) != 0) { mbedtls_aes_free(&ctx); return false; }
    bool ok = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, (size_t)cipher_len, iv, cipher, buf) == 0;
    mbedtls_aes_free(&ctx);
    if (!ok) return false;

    uint8_t pad = buf[cipher_len - 1];
    if (pad == 0 || pad > 16 || pad > cipher_len) return false; // невалидный padding - неверный ключ/данные
    size_t plain_len = (size_t)cipher_len - pad;
    if (plain_len >= out_max) return false;

    memcpy(out, buf, plain_len);
    out[plain_len] = 0;
    return true;
}

bool k85_pwmgr_get_password(int idx, char *out, size_t out_size) {
    if (!s_unlocked) return false;
    if (idx < 0 || idx >= g_config.pw_entries_count) return false;

    k85_pwentry_t *e = &g_config.pw_entries[idx];
    return aes_decrypt(s_session_key, e->iv, e->ciphertext, e->ciphertext_len, out, out_size);
}

bool k85_pwmgr_add(const char *name, const char *username, const char *password) {
    if (!s_unlocked) return false;
    if (g_config.pw_entries_count >= K85_MAX_PW_ENTRIES) return false;

    int i = g_config.pw_entries_count;
    k85_pwentry_t *e = &g_config.pw_entries[i];

    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->username, sizeof(e->username), "%s", username);
    esp_fill_random(e->iv, sizeof(e->iv));

    int enc_len = 0;
    if (!aes_encrypt(s_session_key, e->iv, (const uint8_t *)password, strlen(password),
                      e->ciphertext, sizeof(e->ciphertext), &enc_len)) {
        return false;
    }
    e->ciphertext_len = enc_len;

    g_config.pw_entries_count++;
    k85_config_save();
    return true;
}

bool k85_pwmgr_remove(int idx) {
    if (idx < 0 || idx >= g_config.pw_entries_count) return false;
    for (int i = idx; i < g_config.pw_entries_count - 1; i++) {
        g_config.pw_entries[i] = g_config.pw_entries[i + 1];
    }
    g_config.pw_entries_count--;
    k85_config_save();
    return true;
}
