#pragma once
#include <stdint.h>
#include <stdbool.h>
// K85BL3: talks to the bootloader menu through one record in the spare flash sector 0x12000.
#define K85_BOOT_REQ_MENU      0x20U   // show the boot menu on the next boot
#define K85_BOOT_REQ_MENU_ALT  0x21U   // same, Alt Firmware highlighted (a .bin was just flashed)
// Choice made in the bootloader menu: 0 k85OS, 1 BIOS, 2 Test, 4 Rec; -1 = the menu was not shown.
// Consumes the record (one-shot).
int  k85_boot_request_take(void);
void k85_boot_request_post(uint32_t cmd);
bool k85_boot_request_menu(void);   // obsolete (always false), kept for older boot_screen.cpp patches
static inline void k85_boot_ui_sync(void) {}   // obsolete: colours are edited in the bootloader now ("Custom colors")