#pragma once

#include <stdbool.h>
#include <stddef.h>

// Runs a script file (path relative to /littlefs or absolute /littlefs/...).
// Output of all commands is appended to out (may be nullptr).
// Returns number of lines processed, or -1 on error (message goes to out).
int k85_script_run_file(const char *path, char *out, size_t out_size);

// Executes a single line: variable expansion, then a normal shell command.
// out must be a valid buffer.
void k85_script_exec(const char *line, char *out, size_t out_size);

// Extra shell commands (echo, set, alias, unalias, sleep, sh, cron, ftp,
// ftppass) and alias lookup. Called by k85_shell_run_command() for commands
// it does not know. Returns true if the command was handled.
bool k85_ext_run_command(const char *cmd, const char *arg, char *out, size_t out_size);

// Call once from app_main() after LittleFS + WiFi init, before the UI loop.
// Starts (in a background task): cron, /littlefs/startup.sh, FTP autostart.
void k85_automation_boot(void);
