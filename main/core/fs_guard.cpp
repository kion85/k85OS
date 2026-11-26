// K85SEC6: the shared list of protected files (see fs_guard.h)
#include "fs_guard.h"
#include <cctype>
#include <cstring>

static bool has_ci(const char *s, const char *needle) {
    const size_t n = strlen(needle);
    if (n == 0) return true;
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == (unsigned char)needle[i]) i++;
        if (i == n) return true;
        if (!s[i]) return false;
    }
    return false;
}

bool k85_is_blocked_path(const char *path) {
    if (!path || strchr(path, '~')) return true;  // K85SEC9: FAT short names (K85OS_~1.JSO) dodge the name match
    static const char *const blocked[] = {
        "k85_ssh_host_key", "k85_ssh_",
        "k85os_config",      // the config and its .bak / .tmp / .new / .prev copies (WiFi passwords, hashes)
        "k85os_backup",      // Settings -> Backup file
        "k85_svc",           // services file (FTP login hash)
    };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        if (has_ci(path, blocked[i])) return true;
    }
    return false;
}