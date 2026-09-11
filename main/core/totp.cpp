#include "totp.h"
#include "config.h"
#include "log.h"

#include "mbedtls/md.h"

#include <cstring>
#include <cstdio>
#include <ctime>
#include <cstdint>

// RFC4648 base32 decode. Нечувствителен к регистру, игнорирует '=' и пробелы,
// пропускает любые другие невалидные символы (терпимо к опечаткам/пробелам,
// которые люди иногда вставляют при копировании секрета с сайта).
static int base32_decode(const char *in, uint8_t *out, size_t out_max) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    int buffer = 0;
    int bits_left = 0;
    size_t count = 0;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == '=' || c == ' ' || c == '-') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        const char *pos = strchr(alphabet, c);
        if (!pos) continue;
        int val = (int)(pos - alphabet);
        buffer = (buffer << 5) | val;
        bits_left += 5;
        if (bits_left >= 8) {
            if (count >= out_max) break;
            out[count++] = (uint8_t)((buffer >> (bits_left - 8)) & 0xFF);
            bits_left -= 8;
        }
    }
    return (int)count;
}

static bool hmac_sha1(const uint8_t *key, size_t key_len,
                       const uint8_t *msg, size_t msg_len, uint8_t out[20]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!info) return false;
    return mbedtls_md_hmac(info, key, key_len, msg, msg_len, out) == 0;
}

bool k85_totp_add(const char *name, const char *secret_base32) {
    if (g_config.totp_entries_count >= K85_MAX_TOTP_ENTRIES) return false;

    uint8_t test[64];
    if (base32_decode(secret_base32, test, sizeof(test)) <= 0) {
        k85_log("totp: invalid base32 secret, not added");
        return false;
    }

    int i = g_config.totp_entries_count;
    snprintf(g_config.totp_entries[i].name, sizeof(g_config.totp_entries[i].name), "%s", name);
    snprintf(g_config.totp_entries[i].secret_base32, sizeof(g_config.totp_entries[i].secret_base32), "%s", secret_base32);
    g_config.totp_entries_count++;
    k85_config_save();
    return true;
}

bool k85_totp_remove(int idx) {
    if (idx < 0 || idx >= g_config.totp_entries_count) return false;
    for (int i = idx; i < g_config.totp_entries_count - 1; i++) {
        g_config.totp_entries[i] = g_config.totp_entries[i + 1];
    }
    g_config.totp_entries_count--;
    k85_config_save();
    return true;
}

int k85_totp_count(void) { return g_config.totp_entries_count; }

const char *k85_totp_name(int idx) {
    if (idx < 0 || idx >= g_config.totp_entries_count) return "";
    return g_config.totp_entries[idx].name;
}

bool k85_totp_generate_code(int idx, char *out_code, size_t out_size) {
    if (idx < 0 || idx >= g_config.totp_entries_count) return false;

    uint8_t secret[64];
    int secret_len = base32_decode(g_config.totp_entries[idx].secret_base32, secret, sizeof(secret));
    if (secret_len <= 0) return false;

    time_t now = time(nullptr);
    uint64_t counter = (uint64_t)now / 30;
    uint8_t counter_bytes[8];
    for (int i = 7; i >= 0; i--) {
        counter_bytes[i] = (uint8_t)(counter & 0xFF);
        counter >>= 8;
    }

    uint8_t digest[20];
    if (!hmac_sha1(secret, (size_t)secret_len, counter_bytes, sizeof(counter_bytes), digest)) return false;

    int offset = digest[19] & 0x0F;
    uint32_t binary = ((uint32_t)(digest[offset] & 0x7F) << 24)
                     | ((uint32_t)digest[offset + 1] << 16)
                     | ((uint32_t)digest[offset + 2] << 8)
                     | (uint32_t)digest[offset + 3];
    uint32_t code = binary % 1000000u;
    snprintf(out_code, out_size, "%06u", (unsigned)code);
    return true;
}

int k85_totp_seconds_remaining(void) {
    time_t now = time(nullptr);
    return 30 - (int)(now % 30);
}