// Кастомный аллокатор для wolfSSL/wolfCrypt/wolfSSH (XMALLOC_USER).
// Все выделения библиотеки идут через PSRAM вместо internal RAM, чтобы не
// конкурировать за фрагментированный internal-хип с WiFi/mbedTLS/остальной
// системой — именно эта конкуренция роняла HandshakeInfoNew в internal.c.
#include <stddef.h>
#include "esp_heap_caps.h"

void *XMALLOC(size_t n, void *heap, int type) {
    (void)heap; (void)type;
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

void *XREALLOC(void *p, size_t n, void *heap, int type) {
    (void)heap; (void)type;
    void *np = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    if (!np) np = heap_caps_realloc(p, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return np;
}

void XFREE(void *p, void *heap, int type) {
    (void)heap; (void)type;
    heap_caps_free(p);
}
