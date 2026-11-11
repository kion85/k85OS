// k85OS USB drive: the SD card is exposed to the computer as a USB flash drive (TinyUSB MSC).
// While active the stick does not touch the card; A+B ejects, shuts USB down and restarts.
#include "usb_drive.h"
#include "soc/soc_caps.h"
#include "input.h"
#include "power.h"
#include "theme.h"
#include "rec_pins.h"

#include "M5Unified.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

#define UD_MOUNT "/sdcard"

// same SD wiring and bus (SPI2) as the Rec app and the backup feature
static bool ud_sd_mount(sdmmc_card_t **card) {
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
    if (esp_vfs_fat_sdspi_mount(UD_MOUNT, &host, &sl, &mc, card) != ESP_OK) {
        spi_bus_free((spi_host_device_t)host.slot);
        return false;
    }
    return true;
}

static void ud_sd_unmount(sdmmc_card_t *card) {
    esp_vfs_fat_sdcard_unmount(UD_MOUNT, card);
    spi_bus_free(SPI2_HOST);
}

// ---------------- tiny UI ----------------
static void ud_msg(const char *title, const char *l1, const char *l2, const char *l3, const char *hint) {
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

static void ud_status(const char *s, uint32_t col) {
    const uint32_t bg = k85_get_bg();
    M5.Display.fillRect(0, 84, M5.Display.width(), 16, bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(col, bg);
    M5.Display.setCursor(6, 88);
    M5.Display.print(s);
}

// true = B pressed, false = A pressed / A+B held
static bool ud_wait_b(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) { k85_wake_screen(); return true; }
        if (k85_btn_a_pressed()) { k85_wake_screen(); return false; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void ud_info(const char *title, const char *l1, const char *l2) {
    ud_msg(title, l1, l2, nullptr, "any key");
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed() || k85_btn_b_pressed()) { k85_wake_screen(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_run_usb_drive(void) {
    ud_msg("USB drive", "The SD card will appear as a", "flash drive on the computer.", "USB flash/monitor is off meanwhile", "B=start  A=cancel");
    if (!ud_wait_b()) return;
    ud_msg("USB drive", "Starting ...", nullptr, nullptr, "");

    sdmmc_card_t *card = nullptr;
    if (!ud_sd_mount(&card)) { ud_info("No SD card", "Insert a FAT32 card", "and try again"); return; }

    tinyusb_msc_sdmmc_config_t mcfg = {};
    mcfg.card = card;
    esp_err_t e = tinyusb_msc_storage_init_sdmmc(&mcfg);
    if (e == ESP_OK) {
        tinyusb_config_t tcfg = {};
        tcfg.external_phy = false;
        e = tinyusb_driver_install(&tcfg);
        if (e != ESP_OK) tinyusb_msc_storage_deinit();
    }
    if (e != ESP_OK) {
        ud_sd_unmount(card);
        ud_info("USB error", "Could not start USB", "(port busy?)");
        return;
    }

    char l1[44];
    snprintf(l1, sizeof(l1), "%.5s  %u MB", card->cid.name,
             (unsigned)(((uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size) >> 20));
    ud_msg("USB drive", l1, "Connect the USB cable to a PC.", "Eject the drive on the PC first!", "A+B (hold) = exit");

    bool first = true, last = false;
    int tick = 0;
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        bool conn = tud_mounted();
        if (first || conn != last) {
            first = false;
            last = conn;
            ud_status(conn ? "PC connected" : "Waiting for the PC ...", conn ? 0x00FF00 : 0xFFFF00);
        }
        if (++tick >= 50) { tick = 0; k85_wake_screen(); }      // keep the screen / USB alive (no idle sleep)
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ud_msg("USB drive", "Ejecting ...", nullptr, nullptr, "");
    tinyusb_msc_storage_deinit();
    tinyusb_driver_uninstall();
    ud_sd_unmount(card);
    ud_msg("USB drive", "Restarting ...", nullptr, nullptr, "");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}