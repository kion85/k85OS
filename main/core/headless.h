#pragma once
#include <stddef.h>
#include <stdbool.h>
// K85HL1: no-display mode. The screen is drawn into a RAM buffer and shown in a browser:
// open AP k85OS-XXXX, http://192.168.4.1 , one session, A+B on the stick confirms the login.
bool k85_headless_pre(void);     // app_main: right after the config is loaded, before WiFi init
void k85_headless_start(void);   // app_main: starts the AP and the web server
bool k85_headless_active(void);
void k85_mem_report(char *out, size_t n);   // shell: mem (free internal RAM / PSRAM + change since the last call)
void k85_headless_log_ram(const char *tag);
bool k85_headless_can_keep_wifi(void);      // BLE tools: true = enough internal RAM to leave WiFi/AP running
bool k85_headless_bios_requested(void);     // main loop: the web page asked for the BIOS (one-shot)
void k85_headless_cmd(const char *arg, char *out, size_t out_size);   // shell: headless [on|off]