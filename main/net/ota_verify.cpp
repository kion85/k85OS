#define HAVE_ED25519
#define WOLFSSL_SHA512
#include "ota_verify.h"
#include "log.h"

#include "wolfssl/wolfcrypt/ed25519.h"

#include <cstring>
#include <cstdlib>

// Публичный ключ Ed25519 - сгенерирован один раз офлайн, приватная половина
// НИКОГДА не попадает на устройство и не в этот репозиторий. Замена этого
// ключа возможна только выпуском новой прошивки - если потеряешь приватный
// ключ, старые устройства не смогут проверить будущие релизы, подписанные
// новым ключом, пока сами не обновятся вручную (например, через веб-хотспот
// без проверки подписи как аварийный путь).
static const uint8_t K85_OTA_PUBLIC_KEY[32] = {
    0xe8, 0x72, 0xa0, 0xe7, 0xf4, 0xa3, 0xb8, 0x3a, 0x9a, 0x5a, 0x0e, 0xd2,
    0xc8, 0x4d, 0x30, 0xcd, 0x60, 0x15, 0x82, 0xa0, 0x94, 0xe5, 0x7a, 0xfe,
    0x8a, 0x0f, 0xca, 0x47, 0x89, 0xfd, 0xac, 0x2a
};

bool k85_ota_verify_signature(const uint8_t *data, size_t data_len, const uint8_t *signature) {
    ed25519_key key;
    if (wc_ed25519_init(&key) != 0) {
        k85_log("ota_verify: ed25519_init failed");
        return false;
    }

    int ret = wc_ed25519_import_public(K85_OTA_PUBLIC_KEY, sizeof(K85_OTA_PUBLIC_KEY), &key);
    if (ret != 0) {
        k85_log("ota_verify: import_public failed, ret=%d", ret);
        wc_ed25519_free(&key);
        return false;
    }

    int verified = 0;
    ret = wc_ed25519_verify_msg(signature, 64, data, (word32)data_len, &verified, &key);
    wc_ed25519_free(&key);

    if (ret != 0) {
        k85_log("ota_verify: verify_msg failed, ret=%d", ret);
        return false;
    }
    return verified == 1;
}

static int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool k85_ota_verify_signature_hex(const uint8_t *data, size_t data_len, const char *signature_hex) {
    if (!signature_hex || strlen(signature_hex) != 128) {
        k85_log("ota_verify: signature hex length wrong (expected 128)");
        return false;
    }

    uint8_t signature[64];
    for (int i = 0; i < 64; i++) {
        int hi = hex_char_val(signature_hex[i * 2]);
        int lo = hex_char_val(signature_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            k85_log("ota_verify: invalid hex in signature");
            return false;
        }
        signature[i] = (uint8_t)((hi << 4) | lo);
    }

    return k85_ota_verify_signature(data, data_len, signature);
}