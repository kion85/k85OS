// K85BL1: bootloader hook. Hold button A at power-on to go back to the k85OS slot,
// whatever third-party firmware the boot selection points to.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#include "bootloader_flash_priv.h"
#include "bootloader_common.h"
#include "esp_flash_partitions.h"

static const char *TAG = "k85boot";

#define K85_BTN_A_GPIO    11            /* M5StickS3 button A, active low (from M5Unified) */
#define K85_HOLD_MS       600
#define K85_PROJECT_NAME  "k85os_idf"
#define K85_IMG_VALID     0x2U

void bootloader_hooks_include(void) { }  /* makes the linker keep this file */

static bool button_a_held(void)
{
    esp_rom_gpio_pad_select_gpio(K85_BTN_A_GPIO);
    gpio_ll_input_enable(&GPIO, K85_BTN_A_GPIO);
    gpio_ll_pullup_en(&GPIO, K85_BTN_A_GPIO);
    esp_rom_delay_us(5000);
    for (int i = 0; i < K85_HOLD_MS / 10; i++) {
        if (gpio_ll_get_level(&GPIO, K85_BTN_A_GPIO) != 0) return false;   /* released */
        esp_rom_delay_us(10000);
    }
    return true;
}

/* app image: 24 byte header + 8 byte segment header, then esp_app_desc_t (project_name at +48) */
static bool slot_holds_k85os(uint32_t offset)
{
    uint8_t b[112];
    if (bootloader_flash_read(offset, b, sizeof(b), false) != ESP_OK) return false;
    if (b[0] != 0xE9) return false;
    uint32_t magic;
    memcpy(&magic, b + 32, sizeof(magic));
    if (magic != 0xABCD5432U) return false;
    return strncmp((const char *)(b + 80), K85_PROJECT_NAME, 32) == 0;
}

static void switch_to_k85os(void)
{
    uint32_t otadata_off = 0, otadata_size = 0;
    uint32_t slot_off[16];
    bool slot_present[16];
    memset(slot_present, 0, sizeof(slot_present));

    esp_partition_info_t info;
    for (int i = 0; i < 95; i++) {
        if (bootloader_flash_read(ESP_PARTITION_TABLE_OFFSET + (uint32_t)i * sizeof(info), &info, sizeof(info), false) != ESP_OK) break;
        if (info.magic != ESP_PARTITION_MAGIC) break;
        if (info.type == PART_TYPE_APP && (info.subtype & 0x10) == 0x10) {
            int idx = info.subtype & 0x0F;
            slot_off[idx] = info.pos.offset;
            slot_present[idx] = true;
        } else if (info.type == PART_TYPE_DATA && info.subtype == PART_SUBTYPE_DATA_OTA) {
            otadata_off = info.pos.offset;
            otadata_size = info.pos.size;
        }
    }

    int found = -1, count = 0;
    for (int i = 0; i < 16; i++) {
        if (slot_present[i] && slot_holds_k85os(slot_off[i])) { found = i; count++; }
    }
    if (count != 1 || otadata_off == 0 || otadata_size < 0x2000) {
        ESP_LOGW(TAG, "k85OS found in %d slot(s): boot selection left untouched", count);
        return;
    }

    esp_ota_select_entry_t e;
    memset(&e, 0xFF, sizeof(e));
    e.ota_seq = (uint32_t)(found + 1);
    e.ota_state = K85_IMG_VALID;
    e.crc = bootloader_common_ota_select_crc(&e);
    if (bootloader_flash_erase_range(otadata_off, otadata_size) != ESP_OK) { ESP_LOGE(TAG, "otadata erase failed"); return; }
    if (bootloader_flash_write(otadata_off, &e, sizeof(e), false) != ESP_OK) { ESP_LOGE(TAG, "otadata write failed"); return; }
    ESP_LOGW(TAG, "boot selection set to ota_%d (k85OS)", found);
}

void bootloader_after_init(void)
{
    if (!button_a_held()) return;
    ESP_LOGW(TAG, "button A held at power-on");
    switch_to_k85os();
}