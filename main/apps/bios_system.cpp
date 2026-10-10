#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "bios_system.h"
#include "common.h"
#include "input.h"
#include "config.h"
#include "list_menu.h"

#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void show_and_wait(const char *msg) {
    k85_show_message(msg);
    wait_ab_exit();
}

static bool confirm_msg(const char *msg) {
    k85_show_message(msg);
    k85_wait_ab_release();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) return true;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static bool copy_file(const char *src, const char *dst_tmp, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst_tmp, "wb");
    if (!out) { fclose(in); return false; }
    char buf[512];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (ok) {
        unlink(dst);
        ok = (rename(dst_tmp, dst) == 0);
    }
    if (!ok) unlink(dst_tmp);
    return ok;
}

// ======================= Restore points =======================
// Files /littlefs/rp/k85os_config.json.rpNNN[_YYYYMMDD_HHMM]. The name contains
// "k85os_config.json", so shell commands and FTP cannot read or change them.
#define RP_DIR    "/littlefs/rp"
#define RP_PREFIX "k85os_config.json.rp"
#define RP_MAX    8
#define RP_LIST   16

struct RpEntry {
    int n;
    uint32_t size;
    char name[56];
    char label[32];
};

static bool rp_parse(const char *fname, RpEntry *e) {
    size_t pl = strlen(RP_PREFIX);
    if (strncmp(fname, RP_PREFIX, pl) != 0) return false;
    size_t l = strlen(fname);
    if (l >= sizeof(e->name)) return false;
    if (l > 4 && !strcmp(fname + l - 4, ".tmp")) return false;
    const char *p = fname + pl;
    if (*p < '0' || *p > '9') return false;
    e->n = atoi(p);
    snprintf(e->name, sizeof(e->name), "%s", fname);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    const char *u = strchr(p, '_');
    if (u && sscanf(u, "_%4d%2d%2d_%2d%2d", &y, &mo, &d, &h, &mi) == 5)
        snprintf(e->label, sizeof(e->label), "%03d %04d-%02d-%02d %02d:%02d", e->n, y, mo, d, h, mi);
    else
        snprintf(e->label, sizeof(e->label), "%03d (no date)", e->n);
    return true;
}

static int rp_list(RpEntry *out, int max) {
    int n = 0;
    DIR *d = opendir(RP_DIR);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && n < max) {
        RpEntry cur;
        memset(&cur, 0, sizeof(cur));
        if (!rp_parse(e->d_name, &cur)) continue;
        char path[96];
        snprintf(path, sizeof(path), RP_DIR "/%s", cur.name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        cur.size = (uint32_t)st.st_size;
        out[n++] = cur;
    }
    closedir(d);
    for (int i = 1; i < n; i++) {                   // sort by number
        RpEntry t = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].n > t.n) { out[j + 1] = out[j]; j--; }
        out[j + 1] = t;
    }
    return n;
}

static void rp_create(void) {
    mkdir(RP_DIR, 0755);
    RpEntry list[RP_LIST];
    int n = rp_list(list, RP_LIST);
    int next = (n ? list[n - 1].n : 0) + 1;
    if (next > 999) { show_and_wait("Too many restore\npoints (999)\nA+B=back"); return; }

    bool rotated = false;
    while (n >= RP_MAX) {                            // keep at most RP_MAX: drop the oldest
        char path[96];
        snprintf(path, sizeof(path), RP_DIR "/%s", list[0].name);
        unlink(path);
        for (int i = 1; i < n; i++) list[i - 1] = list[i];
        n--;
        rotated = true;
    }

    char stamp[24] = "";
    time_t t = time(nullptr);
    if (t > 1700000000) {
        time_t local = t + (time_t)g_config.utc_offset * 3600;
        struct tm tm;
        gmtime_r(&local, &tm);
        strftime(stamp, sizeof(stamp), "_%Y%m%d_%H%M", &tm);
    }
    char dst[110], tmp[120];
    snprintf(dst, sizeof(dst), RP_DIR "/" RP_PREFIX "%03d%s", next, stamp);
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);

    k85_config_save();                               // make sure the file is current
    if (!copy_file(CONFIG_FILE_PATH, tmp, dst)) { show_and_wait("Could not create\nrestore point\nA+B=back"); return; }
    char msg[96];
    snprintf(msg, sizeof(msg), "Restore point %03d created%s\nA+B=back", next, rotated ? "\n(oldest removed)" : "");
    show_and_wait(msg);
}

static void rp_restore(const RpEntry &e) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Restore point %03d?\nDevice will reboot\nB=confirm A+B=cancel", e.n);
    if (!confirm_msg(msg)) return;
    char src[96];
    snprintf(src, sizeof(src), RP_DIR "/%s", e.name);
    if (!copy_file(src, "/littlefs/k85os_config.json.tmp", CONFIG_FILE_PATH)) { show_and_wait("Restore failed\nA+B=back"); return; }
    k85_show_message("Config restored\nRebooting...");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

void k85_run_restore_points(void) {
    while (true) {
        RpEntry list[RP_LIST];
        int n = rp_list(list, RP_LIST);
        if (n > RP_MAX) n = RP_MAX;                  // show the newest ones only
        char labels[RP_MAX][48];
        const char *items[RP_MAX + 2];
        items[0] = "+ Create restore point";
        for (int i = 0; i < n; i++) {
            snprintf(labels[i], sizeof(labels[i]), "%s  %uK", list[i].label, (unsigned)((list[i].size + 1023) / 1024));
            items[i + 1] = labels[i];
        }
        items[n + 1] = "Back";
        char header[40];
        snprintf(header, sizeof(header), "RESTORE POINTS (%d/%d)", n, RP_MAX);
        int idx = k85_run_list_menu(header, items, n + 2, nullptr);
        if (idx < 0 || idx == n + 1) return;
        if (idx == 0) { rp_create(); continue; }

        const RpEntry &e = list[idx - 1];
        static const char *const actions[3] = { "Restore", "Delete", "Back" };
        int act = k85_run_list_menu(e.label, actions, 3, nullptr);
        if (act == 0) {
            rp_restore(e);
        } else if (act == 1) {
            if (confirm_msg("Delete this restore\npoint?\nB=confirm A+B=cancel")) {
                char path[96];
                snprintf(path, sizeof(path), RP_DIR "/%s", e.name);
                unlink(path);
            }
        }
    }
}

// ======================= Integrity check =======================
enum { SL_EMPTY = 0, SL_OK, SL_MISMATCH, SL_NO_DIGEST, SL_READ_ERR };

struct SlotResult {
    const esp_partition_t *p;
    int state;
    bool running;
    uint8_t digest[32];
    char ver[24];
};

// Hashes the image up to (not including) the appended SHA-256 and compares it with the stored one.
static int check_partition(const esp_partition_t *p, uint8_t *digest_out) {
    esp_partition_pos_t pos = { p->address, p->size };
    esp_image_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    if (esp_image_get_metadata(&pos, &meta) != ESP_OK) return SL_EMPTY;
    uint32_t len = meta.image_len;
    bool appended = meta.image.hash_appended != 0;
    if (len < 64 || len > p->size) return SL_EMPTY;
    uint32_t hash_len = appended ? len - 32 : len;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (uint8_t *)malloc(4096);
    if (!buf) return SL_READ_ERR;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int state = SL_OK;
    if (mbedtls_sha256_starts(&ctx, 0) != 0) state = SL_READ_ERR;
    uint32_t off = 0;
    while (state == SL_OK && off < hash_len) {
        uint32_t n = hash_len - off < 4096 ? hash_len - off : 4096;
        if (esp_partition_read(p, off, buf, n) != ESP_OK) { state = SL_READ_ERR; break; }
        if (mbedtls_sha256_update(&ctx, buf, n) != 0) { state = SL_READ_ERR; break; }
        off += n;
        if ((off & 0x3FFFF) == 0) vTaskDelay(1);
    }
    if (state == SL_OK && mbedtls_sha256_finish(&ctx, digest_out) != 0) state = SL_READ_ERR;
    mbedtls_sha256_free(&ctx);

    if (state == SL_OK) {
        if (!appended) {
            state = SL_NO_DIGEST;
        } else {
            uint8_t stored[32];
            if (esp_partition_read(p, len - 32, stored, 32) != ESP_OK) state = SL_READ_ERR;
            else state = memcmp(stored, digest_out, 32) == 0 ? SL_OK : SL_MISMATCH;
        }
    }
    free(buf);
    return state;
}

static const char *state_text(int s) {
    switch (s) {
        case SL_OK: return "OK";
        case SL_MISMATCH: return "CORRUPT";
        case SL_NO_DIGEST: return "no hash";
        case SL_READ_ERR: return "read err";
        default: return "empty";
    }
}

void k85_run_integrity_check(void) {
    SlotResult res[4];
    int n = 0;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        if (n < 4) {
            memset(&res[n], 0, sizeof(res[n]));
            res[n].p = p;
            res[n].running = (p == running);
            n++;
        }
        it = esp_partition_next(it);
    }
    if (n == 0) { show_and_wait("No app partitions\nA+B=back"); return; }

    for (int i = 0; i < n; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Checking %s...\n%d/%d", res[i].p->label, i + 1, n);
        k85_show_message(msg);
        res[i].state = check_partition(res[i].p, res[i].digest);
        esp_app_desc_t desc;
        if (res[i].state != SL_EMPTY && esp_ota_get_partition_description(res[i].p, &desc) == ESP_OK)
            snprintf(res[i].ver, sizeof(res[i].ver), "%.20s", desc.version);
    }

    char labels[4][44];
    const char *items[5];
    for (int i = 0; i < n; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%-7s %-8s %s%s", res[i].p->label, state_text(res[i].state), res[i].ver,
                 res[i].running ? " *" : "");
        items[i] = labels[i];
    }
    items[n] = "Back";
    while (true) {
        int idx = k85_run_list_menu("INTEGRITY (* = running)", items, n + 1, nullptr);
        if (idx < 0 || idx >= n) return;
        const SlotResult &r = res[idx];
        char msg[200];
        if (r.state == SL_EMPTY) {
            snprintf(msg, sizeof(msg), "%s @0x%06X\nempty or invalid image\nA+B=back", r.p->label, (unsigned)r.p->address);
        } else {
            char h1[33], h2[33];
            for (int i = 0; i < 16; i++) {
                snprintf(h1 + 2 * i, 3, "%02X", r.digest[i]);
                snprintf(h2 + 2 * i, 3, "%02X", r.digest[16 + i]);
            }
            snprintf(msg, sizeof(msg), "%s: %s\nSHA-256:\n%s\n%s\nA+B=back", r.p->label, state_text(r.state), h1, h2);
        }
        show_and_wait(msg);
    }
}

#pragma GCC diagnostic pop
