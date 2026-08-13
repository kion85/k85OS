#include "lock_auth.h"

#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "esp_random.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#define K85_LOCK_ITERATIONS 10000
#define K85_LOCK_SALT_BYTES 16
#define K85_LOCK_HASH_BYTES 32

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

static void pbkdf2(const char *password, const unsigned char *salt, unsigned char *out_hash) {
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                  (const unsigned char *)password, strlen(password),
                                  salt, K85_LOCK_SALT_BYTES, K85_LOCK_ITERATIONS,
                                  K85_LOCK_HASH_BYTES, out_hash);
}

void k85_lock_hash_password(const char *password, char *out, size_t out_size) {
    unsigned char salt[K85_LOCK_SALT_BYTES];
    esp_fill_random(salt, sizeof(salt));

    unsigned char hash[K85_LOCK_HASH_BYTES];
    pbkdf2(password, salt, hash);

    if (out_size < (K85_LOCK_SALT_BYTES + K85_LOCK_HASH_BYTES) * 2 + 1) {
        out[0] = 0;
        return;
    }
    bytes_to_hex(salt, K85_LOCK_SALT_BYTES, out);
    bytes_to_hex(hash, K85_LOCK_HASH_BYTES, out + K85_LOCK_SALT_BYTES * 2);
}

bool k85_lock_verify_password(const char *password, const char *stored_hex) {
    if (!stored_hex || !stored_hex[0]) return false;
    size_t len = strlen(stored_hex);
    if (len != (K85_LOCK_SALT_BYTES + K85_LOCK_HASH_BYTES) * 2) return false;

    unsigned char salt[K85_LOCK_SALT_BYTES];
    unsigned char stored_hash[K85_LOCK_HASH_BYTES];
    if (!hex_to_bytes(stored_hex, K85_LOCK_SALT_BYTES * 2, salt, sizeof(salt))) return false;
    if (!hex_to_bytes(stored_hex + K85_LOCK_SALT_BYTES * 2, K85_LOCK_HASH_BYTES * 2, stored_hash, sizeof(stored_hash))) return false;

    unsigned char computed[K85_LOCK_HASH_BYTES];
    pbkdf2(password, salt, computed);

    return memcmp(computed, stored_hash, K85_LOCK_HASH_BYTES) == 0;
}

