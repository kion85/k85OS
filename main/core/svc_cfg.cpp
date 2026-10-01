#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "svc_cfg.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#define SVC_PATH "/littlefs/k85_svc.cfg"
#define SVC_TMP  "/littlefs/k85_svc.tmp"
#define SVC_MAX  1024

static SemaphoreHandle_t svc_mtx(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}

struct SvcLock {
    SvcLock() { xSemaphoreTake(svc_mtx(), portMAX_DELAY); }
    ~SvcLock() { xSemaphoreGive(svc_mtx()); }
};

static char *svc_alloc(size_t n) {
    char *p = (char *)heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    if (!p) p = (char *)calloc(1, n);
    return p;
}

static void svc_read_all(char *buf, size_t cap) {
    buf[0] = 0;
    FILE *f = fopen(SVC_PATH, "rb");
    if (!f) return;
    size_t r = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[r] = 0;
}

// Splits the next line out of *pos. Returns nullptr when no more lines.
static char *next_line(char **pos) {
    char *line = *pos;
    if (!line || !*line) return nullptr;
    char *nl = strchr(line, '\n');
    if (nl) { *nl = 0; *pos = nl + 1; } else { *pos = nullptr; }
    size_t n = strlen(line);
    if (n && line[n - 1] == '\r') line[n - 1] = 0;
    return line;
}

static bool line_has_key(const char *line, const char *key, size_t klen) {
    return strncmp(line, key, klen) == 0 && line[klen] == '=';
}

bool k85_svc_get_str(const char *key, char *out, size_t out_size) {
    if (!key || !out || out_size == 0) return false;
    out[0] = 0;
    char *buf = svc_alloc(SVC_MAX + 1);
    if (!buf) return false;

    bool found = false;
    {
        SvcLock lk;
        svc_read_all(buf, SVC_MAX + 1);
    }
    size_t klen = strlen(key);
    char *pos = buf;
    while (char *line = next_line(&pos)) {
        if (line_has_key(line, key, klen)) {
            snprintf(out, out_size, "%s", line + klen + 1);
            found = out[0] != 0;
            break;
        }
    }
    free(buf);
    return found;
}

void k85_svc_set_str(const char *key, const char *val) {
    if (!key || !*key || strchr(key, '=') || strchr(key, '\n')) return;
    if (val && (strchr(val, '\n') || strchr(val, '\r'))) return;

    char *in = svc_alloc(SVC_MAX + 1);
    char *outb = svc_alloc(SVC_MAX + 1);
    if (!in || !outb) { free(in); free(outb); return; }

    {
        SvcLock lk;
        svc_read_all(in, SVC_MAX + 1);

        size_t used = 0;
        size_t klen = strlen(key);
        char *pos = in;
        while (char *line = next_line(&pos)) {
            if (!line[0] || line_has_key(line, key, klen)) continue;
            int w = snprintf(outb + used, SVC_MAX - used, "%s\n", line);
            if (w > 0 && (size_t)w < SVC_MAX - used) used += (size_t)w;
        }
        if (val && val[0]) {
            int w = snprintf(outb + used, SVC_MAX - used, "%s=%s\n", key, val);
            if (w > 0 && (size_t)w < SVC_MAX - used) used += (size_t)w;
        }

        FILE *f = fopen(SVC_TMP, "wb");
        if (f) {
            bool ok = fwrite(outb, 1, used, f) == used;
            if (fclose(f) != 0) ok = false;
            if (ok) {
                unlink(SVC_PATH);
                if (rename(SVC_TMP, SVC_PATH) != 0) unlink(SVC_TMP);
            } else {
                unlink(SVC_TMP);
            }
        }
    }
    free(in);
    free(outb);
}

bool k85_svc_get_bool(const char *key, bool def) {
    char v[8];
    if (!k85_svc_get_str(key, v, sizeof(v))) return def;
    return v[0] == '1';
}

void k85_svc_set_bool(const char *key, bool v) {
    k85_svc_set_str(key, v ? "1" : "0");
}

#pragma GCC diagnostic pop
