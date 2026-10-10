// K85_SSH_REMOVED_STUB - SSH server removed; only the password hash helper is kept
// (FTP server and the script engine still use k85_ssh_hash_password).
#include "ssh_server.h"
#include "mbedtls/sha256.h"
#include <cstdio>
#include <cstring>

K85SshStartResult k85_ssh_server_start(void) { return K85_SSH_START_TASK_FAILED; }
void k85_ssh_reserve_stack_early(void) {}
void k85_ssh_server_stop(void) {}
bool k85_ssh_server_is_running(void) { return false; }

void k85_ssh_hash_password(const char *password, char *out_hex) {
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char *)password, strlen(password), hash, 0);
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", hash[i]);
}