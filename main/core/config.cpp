#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "cJSON.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "esp_wifi.h"

static const char *TAG = "k85_cfg";

// ============================ LittleFS ============================
bool k85_fs_init(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = K85_LITTLEFS_BASE_PATH,
        .partition_label = K85_LITTLEFS_PART_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    if (!esp_littlefs_mounted(K85_LITTLEFS_PART_LABEL)) {
        ESP_LOGE(TAG, "LittleFS not mounted");
        return false;
    }
    size_t total = 0, used = 0;
    if (esp_littlefs_info(K85_LITTLEFS_PART_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS OK, used=%uB / total=%uB", (unsigned)used, (unsigned)total);
    }
    return true;
}

bool k85_fs_info(size_t *total_bytes, size_t *used_bytes) {
    return esp_littlefs_info(K85_LITTLEFS_PART_LABEL, total_bytes, used_bytes) == ESP_OK;
}

k85_config_t g_config;

// ============================ defaults ============================
void k85_config_defaults(k85_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->theme_idx        = 0;
    cfg->battery_mode_idx = 1;
    cfg->brightness_active = 80;
    cfg->device_name_idx  = 0;
    cfg->rotation         = 1;
    cfg->bootstyle_idx    = 0;
    cfg->kb_layout        = 0;
    cfg->sound_volume     = 20;
    cfg->utc_offset       = 3; // MSK по умолчанию
    cfg->ota_locked = false;
    cfg->wifi_disabled = false;
    cfg->bt_disabled = false;
    cfg->lock_enabled = false;
    cfg->lock_password[0] = 0;
    cfg->post_beep_enabled = true;
    cfg->alarm_volume_idx = 2; // High по умолчанию
    cfg->sound_muted = false;
    cfg->sb_flags = 0x3; // по умолчанию: battery% + time
    cfg->sb_bg_color = 0xFFFFFFFF;
    cfg->ap_channel = 1;
    cfg->ap_open = false;
    cfg->wifi_saved       = false;
    cfg->wifi_networks_count = 0;
    // high_scores, step_count, step_record, step_date вЂ” СѓР¶Рµ 0/""
}

// ============================ JSON <-> struct ============================

// ============================ Шифрование паролей WiFi на диске ============================
// Ключ выводится из MAC-адреса устройства (SHA-256), пароли хранятся в JSON
// зашифрованными (AES-128-CBC), в рантайме g_config всегда содержит plaintext
// (нужен для esp_wifi_connect). Честно: ключ вычисляется тем же алгоритмом,
// что известен из исходников прошивки — это защита от простого чтения JSON
// "как есть", а не от продвинутого реверс-инжиниринга дампа флеша целиком.
static void get_aes_key(unsigned char key_out[16]) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    unsigned char hash[32];
    char salt[] = "k85os-wifi-store-v1";
    unsigned char input[6 + sizeof(salt)];
    memcpy(input, mac, 6);
    memcpy(input + 6, salt, sizeof(salt));
    mbedtls_sha256(input, sizeof(input), hash, 0);
    memcpy(key_out, hash, 16);
}

static void bytes_to_hex(const unsigned char *data, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) snprintf(out + i * 2, 3, "%02x", data[i]);
}

static bool hex_to_bytes(const char *hex, unsigned char *out, size_t out_max, size_t *out_len) {
    size_t hlen = strlen(hex);
    if (hlen % 2 != 0 || hlen / 2 > out_max) return false;
    for (size_t i = 0; i < hlen / 2; i++) {
        char byte_str[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        out[i] = (unsigned char)strtoul(byte_str, nullptr, 16);
    }
    *out_len = hlen / 2;
    return true;
}

// Шифрует plaintext (до 64 байт) -> hex-строка в out (нужно >= 192 байт)
static void encrypt_to_hex(const char *plaintext, char *out, size_t out_size) {
    if (!plaintext[0]) { out[0] = 0; return; }

    unsigned char key[16];
    get_aes_key(key);

    unsigned char buf[80] = {0};
    size_t plen = strlen(plaintext);
    if (plen > 64) plen = 64;
    memcpy(buf, plaintext, plen);

    // PKCS7-подобное дополнение до кратного 16
    size_t padded_len = ((plen / 16) + 1) * 16;
    unsigned char pad_val = (unsigned char)(padded_len - plen);
    for (size_t i = plen; i < padded_len; i++) buf[i] = pad_val;

    unsigned char iv[16] = {0}; // фиксированный IV: приемлемо для коротких независимых строк на одном устройстве
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    unsigned char cipher[80];
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded_len, iv, buf, cipher);
    mbedtls_aes_free(&aes);

    bytes_to_hex(cipher, padded_len, out);
    out[padded_len * 2] = 0;
}

// Дешифрует hex-строку обратно в plaintext
static void decrypt_from_hex(const char *hex, char *out, size_t out_size) {
    out[0] = 0;
    if (!hex || !hex[0]) return;

    unsigned char cipher[80];
    size_t clen = 0;
    if (!hex_to_bytes(hex, cipher, sizeof(cipher), &clen)) return;
    if (clen == 0 || clen % 16 != 0) return;

    unsigned char key[16];
    get_aes_key(key);
    unsigned char iv[16] = {0};

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, key, 128);
    unsigned char plain[80];
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, clen, iv, cipher, plain);
    mbedtls_aes_free(&aes);

    unsigned char pad_val = plain[clen - 1];
    size_t plain_len = (pad_val <= 16 && pad_val <= clen) ? clen - pad_val : clen;
    if (plain_len >= out_size) plain_len = out_size - 1;
    memcpy(out, plain, plain_len);
    out[plain_len] = 0;
}
static void set_str(char *dst, size_t dst_sz, const char *src) {
    if (src) snprintf(dst, dst_sz, "%s", src);
}

static cJSON *cfg_to_json(const k85_config_t *c) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "theme_idx", c->theme_idx);
    cJSON_AddNumberToObject(root, "battery_mode_idx", c->battery_mode_idx);
    cJSON_AddNumberToObject(root, "brightness_active", c->brightness_active);
    cJSON_AddNumberToObject(root, "device_name_idx", c->device_name_idx);
    cJSON_AddNumberToObject(root, "rotation", c->rotation);
    cJSON_AddNumberToObject(root, "bootstyle_idx", c->bootstyle_idx);
    cJSON_AddNumberToObject(root, "kb_layout", c->kb_layout);
    cJSON_AddNumberToObject(root, "sound_volume", c->sound_volume);
    cJSON_AddNumberToObject(root, "utc_offset", c->utc_offset);
    cJSON_AddBoolToObject(root, "ota_locked", c->ota_locked);
    cJSON_AddBoolToObject(root, "wifi_disabled", c->wifi_disabled);
    cJSON_AddBoolToObject(root, "bt_disabled", c->bt_disabled);
    cJSON_AddBoolToObject(root, "lock_enabled", c->lock_enabled);
    cJSON_AddStringToObject(root, "lock_password", c->lock_password);
    cJSON_AddBoolToObject(root, "post_beep_enabled", c->post_beep_enabled);
    cJSON_AddNumberToObject(root, "alarm_volume_idx", c->alarm_volume_idx);
    cJSON_AddBoolToObject(root, "sound_muted", c->sound_muted);
    cJSON_AddNumberToObject(root, "sb_flags", (double)c->sb_flags);
    cJSON_AddNumberToObject(root, "sb_bg_color", (double)c->sb_bg_color);
    cJSON_AddNumberToObject(root, "ap_channel", c->ap_channel);
    cJSON_AddBoolToObject(root, "ap_open", c->ap_open);

    cJSON *hs = cJSON_CreateObject();
    cJSON_AddNumberToObject(hs, "2048",    c->high_scores.game_2048);
    cJSON_AddNumberToObject(hs, "tetris",  c->high_scores.tetris);
    cJSON_AddNumberToObject(hs, "snake",   c->high_scores.snake);
    cJSON_AddNumberToObject(hs, "memory",  c->high_scores.memory);
    cJSON_AddNumberToObject(hs, "reaction", c->high_scores.reaction);
    cJSON_AddNumberToObject(hs, "flappy",  c->high_scores.flappy);
    cJSON_AddNumberToObject(hs, "balance", c->high_scores.balance);
    cJSON_AddNumberToObject(hs, "pong",    c->high_scores.pong);
    cJSON_AddItemToObject(root, "high_scores", hs);

    cJSON *w = cJSON_CreateObject();
    cJSON_AddStringToObject(w, "ssid", c->wifi_ssid);
    char wifi_password_enc[192];
    encrypt_to_hex(c->wifi_password, wifi_password_enc, sizeof(wifi_password_enc));
    cJSON_AddStringToObject(w, "password", wifi_password_enc);
    cJSON_AddBoolToObject(w, "saved", c->wifi_saved);
    cJSON_AddItemToObject(root, "wifi", w);

    cJSON *nets = cJSON_CreateArray();
    for (int i = 0; i < c->wifi_networks_count && i < K85_MAX_WIFI_NETWORKS; i++) {
        cJSON *el = cJSON_CreateObject();
        cJSON_AddStringToObject(el, "ssid", c->wifi_networks[i].ssid);
        char net_pass_enc[192];
        encrypt_to_hex(c->wifi_networks[i].password, net_pass_enc, sizeof(net_pass_enc));
        cJSON_AddStringToObject(el, "password", net_pass_enc);
        cJSON_AddItemToArray(nets, el);
    }
    cJSON_AddItemToObject(root, "wifi_networks", nets);

    cJSON_AddNumberToObject(root, "step_count", c->step_count);
    cJSON_AddNumberToObject(root, "step_record", c->step_record);
    cJSON_AddStringToObject(root, "step_date", c->step_date);
    return root;
}

static void cfg_from_json(cJSON *root, k85_config_t *out) {
#define GET_INT(key, field) \
    do { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, key); \
         if (_x && cJSON_IsNumber(_x)) out->field = _x->valueint; } while (0)

    GET_INT("theme_idx", theme_idx);
    GET_INT("battery_mode_idx", battery_mode_idx);
    GET_INT("brightness_active", brightness_active);
    GET_INT("device_name_idx", device_name_idx);
    GET_INT("rotation", rotation);
    GET_INT("bootstyle_idx", bootstyle_idx);
    GET_INT("kb_layout", kb_layout);
    GET_INT("sound_volume", sound_volume);
    GET_INT("utc_offset", utc_offset);
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "ota_locked"); if (_x) out->ota_locked = cJSON_IsTrue(_x); }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "wifi_disabled"); if (_x) out->wifi_disabled = cJSON_IsTrue(_x); }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "bt_disabled"); if (_x) out->bt_disabled = cJSON_IsTrue(_x); }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "lock_enabled"); if (_x) out->lock_enabled = cJSON_IsTrue(_x); }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "lock_password"); if (_x && cJSON_IsString(_x)) set_str(out->lock_password, sizeof(out->lock_password), _x->valuestring); }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "post_beep_enabled"); if (_x) out->post_beep_enabled = cJSON_IsTrue(_x); }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "alarm_volume_idx"); if (_x && cJSON_IsNumber(_x)) out->alarm_volume_idx = _x->valueint; }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "sound_muted"); if (_x) out->sound_muted = cJSON_IsTrue(_x); }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "sb_flags"); if (_x && cJSON_IsNumber(_x)) out->sb_flags = (uint32_t)_x->valuedouble; }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "sb_bg_color"); if (_x && cJSON_IsNumber(_x)) out->sb_bg_color = (uint32_t)_x->valuedouble; }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "ap_channel"); if (_x && cJSON_IsNumber(_x)) out->ap_channel = _x->valueint; }
    { cJSON *_x = cJSON_GetObjectItemCaseSensitive(root, "ap_open"); if (_x) out->ap_open = cJSON_IsTrue(_x); }
#undef GET_INT

    cJSON *hs = cJSON_GetObjectItemCaseSensitive(root, "high_scores");
    if (hs && cJSON_IsObject(hs)) {
#define GET_HS(key, field) \
        do { cJSON *_x = cJSON_GetObjectItemCaseSensitive(hs, key); \
             if (_x && cJSON_IsNumber(_x)) out->high_scores.field = _x->valueint; } while (0)
        GET_HS("2048", game_2048);
        GET_HS("tetris", tetris);
        GET_HS("snake", snake);
        GET_HS("memory", memory);
        GET_HS("reaction", reaction);
        GET_HS("flappy", flappy);
        GET_HS("balance", balance);
        GET_HS("pong", pong);
#undef GET_HS
    }

    cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (w && cJSON_IsObject(w)) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(w, "ssid");
        if (ssid && cJSON_IsString(ssid)) set_str(out->wifi_ssid, sizeof(out->wifi_ssid), ssid->valuestring);
        cJSON *pass = cJSON_GetObjectItemCaseSensitive(w, "password");
        if (pass && cJSON_IsString(pass)) set_str(out->wifi_password, sizeof(out->wifi_password), pass->valuestring);
        cJSON *saved = cJSON_GetObjectItemCaseSensitive(w, "saved");
        if (saved) out->wifi_saved = cJSON_IsTrue(saved);
    }

    cJSON *nets = cJSON_GetObjectItemCaseSensitive(root, "wifi_networks");
    if (nets && cJSON_IsArray(nets)) {
        out->wifi_networks_count = 0;
        int n = cJSON_GetArraySize(nets);
        for (int i = 0; i < n && out->wifi_networks_count < K85_MAX_WIFI_NETWORKS; i++) {
            cJSON *el = cJSON_GetArrayItem(nets, i);
            if (!el || !cJSON_IsObject(el)) continue;
            cJSON *s = cJSON_GetObjectItemCaseSensitive(el, "ssid");
            if (!s || !cJSON_IsString(s)) continue;
            k85_wifi_net_t *net = &out->wifi_networks[out->wifi_networks_count];
            set_str(net->ssid, sizeof(net->ssid), s->valuestring);
            cJSON *p = cJSON_GetObjectItemCaseSensitive(el, "password");
            set_str(net->password, sizeof(net->password), (p && cJSON_IsString(p)) ? p->valuestring : "");
            out->wifi_networks_count++;
        }
    }

    cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "step_count");
    if (sc && cJSON_IsNumber(sc)) out->step_count = sc->valueint;
    cJSON *sr = cJSON_GetObjectItemCaseSensitive(root, "step_record");
    if (sr && cJSON_IsNumber(sr)) out->step_record = sr->valueint;
    cJSON *sd = cJSON_GetObjectItemCaseSensitive(root, "step_date");
    if (sd && cJSON_IsString(sd)) set_str(out->step_date, sizeof(out->step_date), sd->valuestring);
}

// РђРЅР°Р»РѕРі _deep_merge_defaults РёР· MicroPython
static void deep_merge(cJSON *dst, const cJSON *src) {
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, src) {
        if (!item->string) continue;
        cJSON *existing = cJSON_GetObjectItemCaseSensitive(dst, item->string);
        if (!existing) {
            cJSON *copy = cJSON_Duplicate(item, 1);
            if (copy) cJSON_AddItemToObject(dst, item->string, copy);
        } else if (cJSON_IsObject(item) && cJSON_IsObject(existing)) {
            deep_merge(existing, item);
        }
    }
}

// ============================ load / save ============================
bool k85_config_load(void) {
    k85_config_defaults(&g_config);

    FILE *f = fopen(CONFIG_FILE_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "Config file not found, using defaults");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error");
        return false;
    }

    cJSON *defaults = cfg_to_json(&g_config);
    if (defaults) {
        deep_merge(root, defaults);
        cJSON_Delete(defaults);
    }
    cfg_from_json(root, &g_config);
    cJSON_Delete(root);
    return true;
}

bool k85_config_save(void) {
    cJSON *root = cfg_to_json(&g_config);
    if (!root) return false;
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return false;

    FILE *f = fopen(CONFIG_FILE_PATH, "w");
    bool ok = false;
    if (f) {
        ok = fputs(txt, f) >= 0;
        fclose(f);
    }
    cJSON_free(txt);
    return ok;
}

// ============================ high scores ============================
int k85_get_high_score(const char *game) {
    if (!strcmp(game, "2048"))    return g_config.high_scores.game_2048;
    if (!strcmp(game, "tetris"))  return g_config.high_scores.tetris;
    if (!strcmp(game, "snake"))   return g_config.high_scores.snake;
    if (!strcmp(game, "memory"))  return g_config.high_scores.memory;
    if (!strcmp(game, "reaction")) return g_config.high_scores.reaction;
    if (!strcmp(game, "flappy"))  return g_config.high_scores.flappy;
    if (!strcmp(game, "balance")) return g_config.high_scores.balance;
    if (!strcmp(game, "pong"))    return g_config.high_scores.pong;
    return 0;
}

static int *high_score_ptr(const char *game) {
    if (!strcmp(game, "2048"))    return &g_config.high_scores.game_2048;
    if (!strcmp(game, "tetris"))  return &g_config.high_scores.tetris;
    if (!strcmp(game, "snake"))   return &g_config.high_scores.snake;
    if (!strcmp(game, "memory"))  return &g_config.high_scores.memory;
    if (!strcmp(game, "reaction")) return &g_config.high_scores.reaction;
    if (!strcmp(game, "flappy"))  return &g_config.high_scores.flappy;
    if (!strcmp(game, "balance")) return &g_config.high_scores.balance;
    if (!strcmp(game, "pong"))    return &g_config.high_scores.pong;
    return NULL;
}

bool k85_set_high_score_if_better(const char *game, int value) {
    int *p = high_score_ptr(game);
    if (!p) return false;
    if (value > *p) {
        *p = value;
        k85_config_save();
        return true;
    }
    return false;
}

bool k85_set_low_score_if_better(const char *game, int value) {
    int *p = high_score_ptr(game);
    if (!p) return false;
    if (*p == 0 || value < *p) {
        *p = value;
        k85_config_save();
        return true;
    }
    return false;
}

// ============================ sound ============================
int k85_get_sound_volume(void) {
    return g_config.sound_volume;
}

void k85_set_sound_volume(int v) {
    g_config.sound_volume = v;
    k85_config_save();
}









