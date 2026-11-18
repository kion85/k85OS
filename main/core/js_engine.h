#pragma once
#include <stddef.h>
#include <stdbool.h>
// K85JS1: mini JavaScript (Elk). ui=true: screen + buttons allowed (menu), ui=false: headless (shell/cron/startup.sh).
bool k85_js_run_file(const char *path, char *out, size_t out_size, bool ui);
bool k85_js_run_code(const char *code, char *out, size_t out_size, bool ui);
bool k85_js_running(void);
void k85_run_js_menu(void);   // main menu -> JS