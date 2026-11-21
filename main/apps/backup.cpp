// k85OS settings backup: copies k85os_config.json to / from the SD card (Settings -> Backup).
#include "backup.h"
#include "config.h"
#include "theme.h"
#include "input.h"
#include "power.h"
#include "list_menu.h"
#include "rec_pins.h"
#include "text_input.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include <cstddef>
#include <cstdint>

#include "M5Unified.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#define BK_MOUNT "/sdcard"
#define BK_FILE  "/sdcard/k85os_backup.json"
#define BK_NEW   "/littlefs/k85os_config.new"
#define BK_PREV  "/littlefs/k85os_config.prev"
#define BK_MAX   100000

// same SD wiring / bus as the Rec app (SPI2), own copy so rec.cpp stays untouched
static bool bk_sd_mount(sdmmc_card_t **card) {
    esp_vfs_fat_mount_config_t mc = {};
    mc.format_if_mount_failed = false;
    mc.max_files = 4;
    mc.allocation_unit_size = 16 * 1024;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;
    spi_bus_config_t bus = {};
    bus.mosi_io_num = K85_SD_MOSI;
    bus.miso_io_num = K85_SD_MISO;
    bus.sclk_io_num = K85_SD_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t e = spi_bus_initialize((spi_host_device_t)host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
    sdspi_device_config_t sl = SDSPI_DEVICE_CONFIG_DEFAULT();
    sl.gpio_cs = (gpio_num_t)K85_SD_CS;
    sl.host_id = (spi_host_device_t)host.slot;
    if (esp_vfs_fat_sdspi_mount(BK_MOUNT, &host, &sl, &mc, card) != ESP_OK) {
        spi_bus_free((spi_host_device_t)host.slot);
        return false;
    }
    return true;
}

static void bk_sd_unmount(sdmmc_card_t *card) {
    esp_vfs_fat_sdcard_unmount(BK_MOUNT, card);
    spi_bus_free(SPI2_HOST);
}

static long bk_file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static bool bk_copy(const char *src, const char *dst, size_t *out_bytes) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    uint8_t *buf = (uint8_t *)malloc(2048);
    if (!buf) { fclose(in); fclose(out); return false; }
    size_t total = 0;
    bool ok = true;
    while (true) {
        size_t n = fread(buf, 1, 2048, in);
        if (n == 0) break;
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
        total += n;
    }
    if (ferror(in)) ok = false;
    free(buf);
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (out_bytes) *out_bytes = total;
    return ok && total > 0;
}

// ---------------- tiny UI ----------------
static void bk_msg(const char *title, const char *l1, const char *l2, const char *l3, const char *hint) {
    const uint32_t bg = k85_get_bg();
    M5.Display.fillScreen(bg);
    M5.Display.setTextColor(k85_get_accent(), bg);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(6, 6);
    M5.Display.print(title);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(k85_get_fg(), bg);
    const char *ls[3] = { l1, l2, l3 };
    for (int i = 0; i < 3; i++) {
        if (!ls[i]) continue;
        M5.Display.setCursor(6, 34 + i * 14);
        M5.Display.print(ls[i]);
    }
    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(6, M5.Display.height() - 12);
    M5.Display.print(hint);
}

// true = B pressed, false = A pressed / A+B held
static bool bk_wait_b(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) { k85_wake_screen(); return true; }
        if (k85_btn_a_pressed()) { k85_wake_screen(); return false; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void bk_wait_any(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed() || k85_btn_b_pressed()) { k85_wake_screen(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void bk_info(const char *title, const char *l1, const char *l2, const char *l3) {
    bk_msg(title, l1, l2, l3, "any key");
    bk_wait_any();
}

// ---------------- encrypted backup (K85SEC7) ----------------
// file: "K85BK1\0\0" | iter u32 | salt 16 | iv 12 | len u32 | tag 16 | ciphertext
// AES-256-GCM, key = PBKDF2-HMAC-SHA256(password, salt, iter). A backup without the magic is an old plain JSON one.
#define BK_ITER 50000
struct BkHdr {
    char magic[8];
    uint32_t iter;
    uint8_t salt[16];
    uint8_t iv[12];
    uint32_t len;
    uint8_t tag[16];
};
static_assert(sizeof(BkHdr) == 60, "BkHdr layout");

static void *bk_alloc(size_t n) {
    if (n == 0) n = 1;
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static void bk_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static bool bk_kdf(const char *pw, const uint8_t *salt, uint32_t iter, uint8_t key[32]) {
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw, strlen(pw),
                                         salt, 16, iter, 32, key) == 0;
}

static bool bk_enc_write(FILE *f, const char *pw, const uint8_t *plain, size_t n) {
    BkHdr h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "K85BK1", 6);
    h.iter = BK_ITER;
    h.len = (uint32_t)n;
    esp_fill_random(h.salt, sizeof(h.salt));
    esp_fill_random(h.iv, sizeof(h.iv));
    uint8_t key[32];
    if (!bk_kdf(pw, h.salt, h.iter, key)) return false;
    uint8_t *ct = (uint8_t *)bk_alloc(n);
    if (!ct) { bk_wipe(key, sizeof(key)); return false; }
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, h.iv, sizeof(h.iv),
                                        (const uint8_t *)&h, offsetof(BkHdr, tag), plain, ct,
                                        sizeof(h.tag), h.tag) == 0;
    mbedtls_gcm_free(&g);
    bk_wipe(key, sizeof(key));
    ok = ok && fwrite(&h, 1, sizeof(h), f) == sizeof(h) && fwrite(ct, 1, n, f) == n;
    free(ct);
    return ok;
}

// out must have room for the ciphertext length; false = wrong password, damaged file or bad header
static bool bk_decrypt(const uint8_t *file, size_t fsz, const char *pw, uint8_t *out, size_t *out_len) {
    BkHdr h;
    if (fsz < sizeof(h)) return false;
    memcpy(&h, file, sizeof(h));
    if (memcmp(h.magic, "K85BK1\0\0", 8) != 0) return false;
    if (h.iter < 1000 || h.iter > 200000) return false;
    if ((size_t)h.len != fsz - sizeof(h)) return false;
    uint8_t key[32];
    if (!bk_kdf(pw, h.salt, h.iter, key)) return false;
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_auth_decrypt(&g, h.len, h.iv, sizeof(h.iv),
                                       (const uint8_t *)&h, offsetof(BkHdr, tag), h.tag, sizeof(h.tag),
                                       file + sizeof(h), out) == 0;
    mbedtls_gcm_free(&g);
    bk_wipe(key, sizeof(key));
    if (ok) *out_len = h.len;
    return ok;
}

// ---------------- save ----------------
static void bk_save(void) {
    bk_msg("Backup", "Saves all settings to the SD", "card, ENCRYPTED with a password", "you choose (do not forget it!)", "B=continue  A=cancel");
    if (!bk_wait_b()) return;
    char pw[64] = "", pw2[64] = "";
    if (!k85_text_input("Backup password (4+)", "", pw, sizeof(pw))) { bk_wipe(pw, sizeof(pw)); return; }
    if (strlen(pw) < 4) { bk_wipe(pw, sizeof(pw)); bk_info("Too short", "Use at least 4 characters", nullptr, nullptr); return; }
    if (!k85_text_input("Repeat password", "", pw2, sizeof(pw2))) { bk_wipe(pw, sizeof(pw)); bk_wipe(pw2, sizeof(pw2)); return; }
    const bool same = (strcmp(pw, pw2) == 0);
    bk_wipe(pw2, sizeof(pw2));
    if (!same) { bk_wipe(pw, sizeof(pw)); bk_info("Passwords differ", "Nothing was saved", nullptr, nullptr); return; }

    bk_msg("Backup", "Encrypting and saving ...", "(takes a few seconds)", nullptr, "");
    k85_config_save();                                  // the file must match the settings in RAM
    const long csz = bk_file_size(CONFIG_FILE_PATH);
    if (csz <= 0 || csz > BK_MAX) { bk_wipe(pw, sizeof(pw)); bk_info("Backup failed", "Config file not readable", nullptr, nullptr); return; }
    uint8_t *plain = (uint8_t *)bk_alloc((size_t)csz);
    if (!plain) { bk_wipe(pw, sizeof(pw)); bk_info("Error", "Not enough memory", nullptr, nullptr); return; }
    FILE *cf = fopen(CONFIG_FILE_PATH, "rb");
    const size_t rd = cf ? fread(plain, 1, (size_t)csz, cf) : 0;
    if (cf) fclose(cf);
    if (rd != (size_t)csz) {
        bk_wipe(plain, (size_t)csz); free(plain); bk_wipe(pw, sizeof(pw));
        bk_info("Backup failed", "Could not read the config", nullptr, nullptr);
        return;
    }
    sdmmc_card_t *card = nullptr;
    if (!bk_sd_mount(&card)) {
        bk_wipe(plain, rd); free(plain); bk_wipe(pw, sizeof(pw));
        bk_info("No SD card", "Insert a FAT32 card", "and try again", nullptr);
        return;
    }
    FILE *o = fopen(BK_FILE, "wb");
    bool ok = o && bk_enc_write(o, pw, plain, rd);
    if (o && fclose(o) != 0) ok = false;
    ok = ok && bk_file_size(BK_FILE) == (long)(sizeof(BkHdr) + rd);
    bk_sd_unmount(card);
    bk_wipe(plain, rd);
    free(plain);
    bk_wipe(pw, sizeof(pw));
    if (ok) {
        char l[40];
        snprintf(l, sizeof(l), "%u bytes written", (unsigned)(sizeof(BkHdr) + rd));
        bk_info("Saved (encrypted)", l, "k85os_backup.json", nullptr);
    } else {
        bk_info("Backup failed", "Could not write the file", "(card full / locked?)", nullptr);
    }
}

// ---------------- restore ----------------
static void bk_restore(void) {
    sdmmc_card_t *card = nullptr;
    if (!bk_sd_mount(&card)) { bk_info("No SD card", "Insert a FAT32 card", "and try again", nullptr); return; }
    const long sz = bk_file_size(BK_FILE);
    if (sz <= 0) { bk_sd_unmount(card); bk_info("No backup", "k85os_backup.json not found", "on the SD card", nullptr); return; }
    if (sz > BK_MAX + 256) { bk_sd_unmount(card); bk_info("Bad file", "File is too big", nullptr, nullptr); return; }

    uint8_t *buf = (uint8_t *)bk_alloc((size_t)sz + 1);
    if (!buf) { bk_sd_unmount(card); bk_info("Error", "Not enough memory", nullptr, nullptr); return; }
    FILE *f = fopen(BK_FILE, "rb");
    const size_t rd = f ? fread(buf, 1, (size_t)sz, f) : 0;
    if (f) fclose(f);
    bk_sd_unmount(card);                                // the whole file is in RAM now
    if (rd != (size_t)sz) { free(buf); bk_info("Bad file", "Could not read the backup", nullptr, nullptr); return; }
    buf[rd] = 0;

    const uint8_t *plain = buf;
    size_t plen = rd;
    uint8_t *dec = nullptr;
    auto cleanup = [&]() {
        if (dec) { bk_wipe(dec, rd + 1); free(dec); dec = nullptr; }
        bk_wipe(buf, rd + 1);
        free(buf);
        buf = nullptr;
    };
    if (rd >= sizeof(BkHdr) && memcmp(buf, "K85BK1\0\0", 8) == 0) {
        char pw[64] = "";
        if (!k85_text_input("Backup password", "", pw, sizeof(pw))) { bk_wipe(pw, sizeof(pw)); cleanup(); return; }
        bk_msg("Restore", "Decrypting ...", "(takes a few seconds)", nullptr, "");
        dec = (uint8_t *)bk_alloc(rd + 1);
        size_t dl = 0;
        const bool dok = dec && bk_decrypt(buf, rd, pw, dec, &dl);
        bk_wipe(pw, sizeof(pw));
        if (!dok) { cleanup(); bk_info("Cannot decrypt", "Wrong password or", "damaged backup", nullptr); return; }
        dec[dl] = 0;
        plain = dec;
        plen = dl;
    }

    // validate: must be a JSON object that looks like a k85OS config
    cJSON *root = cJSON_Parse((const char *)plain);
    const bool valid = root && cJSON_IsObject(root) && cJSON_GetObjectItemCaseSensitive(root, "bootstyle_idx") != nullptr;
    if (root) cJSON_Delete(root);
    if (!valid) { cleanup(); bk_info("Bad file", "Not a k85OS backup", nullptr, nullptr); return; }

    bk_msg("Restore", "Replaces ALL settings with the", "backup, then restarts.", "Current ones kept as .prev", "B=restore  A=cancel");
    if (!bk_wait_b()) { cleanup(); return; }
    bk_msg("Restore", "Restoring ...", nullptr, nullptr, "");

    bool ok = false;
    FILE *w = fopen(BK_NEW, "wb");                      // staged on LittleFS first
    bool wok = w && fwrite(plain, 1, plen, w) == plen;
    if (w && fclose(w) != 0) wok = false;
    if (wok && bk_file_size(BK_NEW) == (long)plen) {
        bk_copy(CONFIG_FILE_PATH, BK_PREV, nullptr);    // safety copy of the current settings
        if (rename(BK_NEW, CONFIG_FILE_PATH) == 0) ok = true;
        else ok = bk_copy(BK_NEW, CONFIG_FILE_PATH, nullptr);
    }
    cleanup();

    if (ok && k85_config_load()) {
        bk_msg("Restored", "Settings loaded.", "Restarting ...", nullptr, "");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        return;
    }
    if (bk_file_size(BK_PREV) > 0) {                    // roll back
        bk_copy(BK_PREV, CONFIG_FILE_PATH, nullptr);
        k85_config_load();
    }
    bk_info("Restore failed", "Old settings kept", nullptr, nullptr);
}
void k85_run_backup_menu(void) {
    const char *items[3] = { "Save to SD", "Restore from SD", "Back" };
    while (true) {
        int c = k85_run_list_menu("Backup", items, 3, nullptr);
        if (c < 0 || c == 2) return;
        if (c == 0) bk_save();
        else bk_restore();
    }
}