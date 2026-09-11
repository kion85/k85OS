#pragma once
// TOTP-аутентификатор (как Google Authenticator): список аккаунтов, живой
// 6-значный код с отсчётом до обновления, добавление нового аккаунта по
// base32-секрету.
void k85_run_totp_auth(void);