#pragma once

#include <stdbool.h>
#include <stddef.h>

// Tiny key=value store in /littlefs/k85_svc.cfg for the automation features
// (startup script, cron, FTP). Kept separate from k85os_config.json.
//
// Keys used: startup (default 1), cron (0), ftp_auto (0), ftp_user, ftp_hash

bool k85_svc_get_str(const char *key, char *out, size_t out_size); // false if missing
void k85_svc_set_str(const char *key, const char *val);            // NULL or "" removes the key
bool k85_svc_get_bool(const char *key, bool def);
void k85_svc_set_bool(const char *key, bool v);
