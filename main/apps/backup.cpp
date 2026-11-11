// k85OS settings backup: copies k85os_config.json to / from the SD card (Settings -> Backup).
#include "backup.h"
#include "config.h"
#include "theme.h"
#include "input.h"
#include "power.h"
#include "list_menu.h"
#include "rec_pins.h"

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

// ---------------- save ----------------
static void bk_save(void) {
    bk_msg("Backup", "Saves all settings to the SD card", "(k85os_backup.json).", "Includes WiFi, TOTP, passwords!", "B=save  A=cancel");
    if (!bk_wait_b()) return;
    bk_msg("Backup", "Saving ...", nullptr, nullptr, "");
    k85_config_save();                                  // the file must match the settings in RAM
    sdmmc_card_t *card = nullptr;
    if (!bk_sd_mount(&card)) { bk_info("No SD card", "Insert a FAT32 card", "and try again", nullptr); return; }
    size_t n = 0;
    bool ok = bk_copy(CONFIG_FILE_PATH, BK_FILE, &n) && bk_file_size(BK_FILE) == bk_file_size(CONFIG_FILE_PATH);
    bk_sd_unmount(card);
    if (ok) {
        char l[40];
        snprintf(l, sizeof(l), "%u bytes written", (unsigned)n);
        bk_info("Saved", l, "k85os_backup.json", nullptr);
    } else {
        bk_info("Backup failed", "Could not write the file", "(card full / locked?)", nullptr);
    }
}

// ---------------- restore ----------------
static void bk_restore(void) {
    sdmmc_card_t *card = nullptr;
    if (!bk_sd_mount(&card)) { bk_info("No SD card", "Insert a FAT32 card", "and try again", nullptr); return; }
    long sz = bk_file_size(BK_FILE);
    if (sz <= 0) { bk_sd_unmount(card); bk_info("No backup", "k85os_backup.json not found", "on the SD card", nullptr); return; }
    if (sz > BK_MAX) { bk_sd_unmount(card); bk_info("Bad file", "File is too big", nullptr, nullptr); return; }

    // validate: must be a JSON object that looks like a k85OS config
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { bk_sd_unmount(card); bk_info("Error", "Not enough memory", nullptr, nullptr); return; }
    FILE *f = fopen(BK_FILE, "rb");
    size_t rd = f ? fread(buf, 1, (size_t)sz, f) : 0;
    if (f) fclose(f);
    buf[rd] = 0;
    cJSON *root = (rd == (size_t)sz) ? cJSON_Parse(buf) : nullptr;
    free(buf);
    bool valid = root && cJSON_IsObject(root) && cJSON_GetObjectItemCaseSensitive(root, "bootstyle_idx") != nullptr;
    if (root) cJSON_Delete(root);
    if (!valid) { bk_sd_unmount(card); bk_info("Bad file", "Not a k85OS backup", nullptr, nullptr); return; }

    bk_msg("Restore", "Replaces ALL settings with the", "backup, then restarts.", "Current ones kept as .prev", "B=restore  A=cancel");
    if (!bk_wait_b()) { bk_sd_unmount(card); return; }
    bk_msg("Restore", "Restoring ...", nullptr, nullptr, "");

    bool ok = false;
    if (bk_copy(BK_FILE, BK_NEW, nullptr) && bk_file_size(BK_NEW) == sz) {   // staged on LittleFS first
        bk_copy(CONFIG_FILE_PATH, BK_PREV, nullptr);                          // safety copy of the current settings
        if (rename(BK_NEW, CONFIG_FILE_PATH) == 0) ok = true;
        else ok = bk_copy(BK_NEW, CONFIG_FILE_PATH, nullptr);
    }
    bk_sd_unmount(card);

    if (ok && k85_config_load()) {
        bk_msg("Restored", "Settings loaded.", "Restarting ...", nullptr, "");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        return;
    }
    if (bk_file_size(BK_PREV) > 0) {                                          // roll back
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