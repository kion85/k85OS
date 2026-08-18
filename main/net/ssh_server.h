#pragma once
#include <stdbool.h>

bool k85_ssh_server_start(void);
void k85_ssh_server_stop(void);
bool k85_ssh_server_is_running(void);

// SHA-256 пароля в hex (64 символа + \0), out_hex должен быть >= 65 байт
void k85_ssh_hash_password(const char *password, char *out_hex);

