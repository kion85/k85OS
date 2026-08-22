#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Общий мьютекс для операций, которые сильно грузят internal RAM (HTTPS-
// запросы через mbedTLS/esp_http_client, BLE-инициализация, SSH-хендшейк).
// Пока такая операция не завершится, следующая ждёт своей очереди — вместо
// того чтобы конкурировать за фрагментированную internal RAM одновременно.
// Именно эта конкуренция раньше давала Malloc failed / CORRUPT HEAP (BLE
// vs WiFi) и TLS handshake timeout (OTA-чекер vs UEFI-темы одновременно).
void k85_heavy_lock_init(void);
bool k85_heavy_lock_take(uint32_t timeout_ms);
void k85_heavy_lock_give(void);

// RAII-обёртка — берёт лок в конструкторе, отпускает в деструкторе на
// ЛЮБОМ пути выхода из функции (включая ранние return). held == false,
// если не дождались лока за timeout_ms — тогда саму операцию выполнять
// не нужно (кто-то другой занят дольше отведённого времени).
struct K85HeavyLockGuard {
    bool held;
    explicit K85HeavyLockGuard(uint32_t timeout_ms = 15000) {
        held = k85_heavy_lock_take(timeout_ms);
    }
    ~K85HeavyLockGuard() {
        if (held) k85_heavy_lock_give();
    }
    K85HeavyLockGuard(const K85HeavyLockGuard&) = delete;
    K85HeavyLockGuard& operator=(const K85HeavyLockGuard&) = delete;
};