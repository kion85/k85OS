#pragma once
#include <stdbool.h>

enum K85SshStartResult { K85_SSH_START_OK, K85_SSH_START_NO_WIFI, K85_SSH_START_ALREADY_RUNNING, K85_SSH_START_TASK_FAILED };
K85SshStartResult k85_ssh_server_start(void);
// Ранняя резервация стека под SSH-таск — вызывать ОДИН РАЗ из app_main(),
// после littlefs init, до WiFi init.
void k85_ssh_reserve_stack_early(void);
void k85_ssh_server_stop(void);
bool k85_ssh_server_is_running(void);

// SHA-256 ?????? ? hex (64 ??????? + \0), out_hex ?????? ???? >= 65 ????
void k85_ssh_hash_password(const char *password, char *out_hex);


