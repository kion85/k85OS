// K85BL3: record exchange with the bootloader menu (spare flash sector 0x12000)
#include "boot_request.h"
#include "esp_flash.h"
#include "esp_log.h"
#include <cstdint>

#define K85_REQ_OFFSET 0x12000U
#define K85_REQ_SIZE   0x1000U
#define K85_REQ_MAGIC  0x4B38354DU    // 'K85M'
#define K85_REQ_BASE   0x10U          // cmd = base + choice

int k85_boot_request_take(void) {
    uint32_t r[4] = {0, 0, 0, 0};
    if (esp_flash_read(NULL, r, K85_REQ_OFFSET, sizeof(r)) != ESP_OK) return -1;
    if (r[0] != K85_REQ_MAGIC || r[2] != ~K85_REQ_MAGIC) return -1;
    esp_flash_erase_region(NULL, K85_REQ_OFFSET, K85_REQ_SIZE);    // one-shot
    if (r[1] >= K85_REQ_BASE && r[1] <= K85_REQ_BASE + 4U) return (int)(r[1] - K85_REQ_BASE);
    return -1;                                                      // menu requests that the bootloader did not consume
}

void k85_boot_request_post(uint32_t cmd) {
    uint32_t r[4] = { K85_REQ_MAGIC, cmd, ~K85_REQ_MAGIC, 0U };
    esp_flash_erase_region(NULL, K85_REQ_OFFSET, K85_REQ_SIZE);
    esp_flash_write(NULL, r, K85_REQ_OFFSET, sizeof(r));
}

bool k85_boot_request_menu(void) { return false; }