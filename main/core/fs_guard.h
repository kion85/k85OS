#pragma once
// K85SEC6: files that must never be readable / writable / deletable through the shell, FTP, the web file manager,
// Lua, the messenger or KStore: SSH keys, the config and every variant of it (.bak .tmp .new .prev), its backups
// and the services file. One shared list; the match is case-insensitive and works on any part of the path.
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

bool k85_is_blocked_path(const char *path);