#include "heavy_lock.h"

static SemaphoreHandle_t s_heavy_mutex = nullptr;

void k85_heavy_lock_init(void) {
    if (!s_heavy_mutex) s_heavy_mutex = xSemaphoreCreateMutex();
}

bool k85_heavy_lock_take(uint32_t timeout_ms) {
    if (!s_heavy_mutex) k85_heavy_lock_init(); // подстраховка, если init() не был вызван явно
    return xSemaphoreTake(s_heavy_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void k85_heavy_lock_give(void) {
    if (s_heavy_mutex) xSemaphoreGive(s_heavy_mutex);
}