#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Files that must never be readable/writable/deletable through shell
// commands or FTP: SSH host key, main config (WiFi passwords, hashes) and
// the services file (FTP login hash).
static inline bool k85_is_blocked_path(const char *path) {
    static const char *const blocked[] = {
        "k85_ssh_host_key", "k85os_config.json", "k85_ssh_", "k85_svc",
    };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        if (strstr(path, blocked[i])) return true;
    }
    return false;
}
