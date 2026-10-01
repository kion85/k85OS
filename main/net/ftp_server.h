#pragma once

#include <stdbool.h>

// Simple FTP server for copying files PC <-> stick over WiFi (STA network only).
// One client at a time, root = /littlefs, login set with the shell command
// `ftppass <user> <password>`. Plain FTP: use only on a trusted network.

enum K85FtpStartResult {
    K85_FTP_START_OK,
    K85_FTP_START_NO_WIFI,
    K85_FTP_START_NO_LOGIN,
    K85_FTP_START_ALREADY_RUNNING,
    K85_FTP_START_FAILED,
};

// idle_timeout = true: server stops itself after 15 min without any client.
K85FtpStartResult k85_ftp_server_start(bool idle_timeout);
void k85_ftp_server_stop(void);
bool k85_ftp_server_is_running(void);
bool k85_ftp_has_login(void);
