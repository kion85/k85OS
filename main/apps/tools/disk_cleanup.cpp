#include "disk_cleanup.h"
#include "common.h"
#include "input.h"
#include "list_menu.h"
#include "../../core/config.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define K85_DC_TOP_N 15
#define K85_DC_MAX_DEPTH 3

struct DcEntry {
    char path[192]; // относительно /littlefs
    long size;
};

static DcEntry s_top[K85_DC_TOP_N];
static int s_top_count = 0;

// Вставка с сохранением убывающей сортировки по size - держим только top-N.
static void dc_consider(const char *relpath, long size) {
    if (s_top_count < K85_DC_TOP_N) {
        int i = s_top_count++;
        while (i > 0 && s_top[i - 1].size < size) {
            s_top[i] = s_top[i - 1];
            i--;
        }
        snprintf(s_top[i].path, sizeof(s_top[i].path), "%s", relpath);
        s_top[i].size = size;
    } else if (size > s_top[K85_DC_TOP_N - 1].size) {
        int i = K85_DC_TOP_N - 1;
        while (i > 0 && s_top[i - 1].size < size) {
            s_top[i] = s_top[i - 1];
            i--;
        }
        snprintf(s_top[i].path, sizeof(s_top[i].path), "%s", relpath);
        s_top[i].size = size;
    }
}

static void dc_scan_dir(const char *full_dir, const char *rel_dir, int depth) {
    if (depth > K85_DC_MAX_DEPTH) return;
    DIR *d = opendir(full_dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char full[300];
        snprintf(full, sizeof(full), "%s/%s", full_dir, ent->d_name);
        char rel[192];
        if (rel_dir[0]) snprintf(rel, sizeof(rel), "%s/%s", rel_dir, ent->d_name);
        else snprintf(rel, sizeof(rel), "%s", ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            dc_scan_dir(full, rel, depth + 1);
        } else {
            dc_consider(rel, (long)st.st_size);
        }
    }
    closedir(d);
}

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static bool delete_confirm(const char *relpath) {
    char full[300];
    snprintf(full, sizeof(full), "/littlefs/%s", relpath);
    char msg[220];
    snprintf(msg, sizeof(msg), "Delete %.60s?\nB=confirm A+B=cancel", relpath);
    k85_show_message(msg);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    bool ok = (remove(full) == 0);
    k85_show_message(ok ? "Deleted!\nA+B=back" : "Delete failed\nA+B=back");
    wait_ab_exit();
    return ok;
}

void k85_run_disk_cleanup(void) {
    while (true) {
        size_t fs_total = 0, fs_used = 0;
        bool fs_ok = k85_fs_info(&fs_total, &fs_used);

        s_top_count = 0;
        dc_scan_dir("/littlefs", "", 0);

        char header[40];
        if (fs_ok) {
            snprintf(header, sizeof(header), "%u/%uKB", (unsigned)(fs_used / 1024), (unsigned)(fs_total / 1024));
        } else {
            snprintf(header, sizeof(header), "N/A");
        }

        if (s_top_count == 0) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Used: %s\nNo files found\nA+B=back", header);
            k85_show_message(msg);
            wait_ab_exit();
            return;
        }

        static char labels[K85_DC_TOP_N][224];
        const char *items[K85_DC_TOP_N + 1];
        for (int i = 0; i < s_top_count; i++) {
            if (s_top[i].size >= 1024) {
                snprintf(labels[i], sizeof(labels[i]), "%.60s (%ldKB)", s_top[i].path, s_top[i].size / 1024);
            } else {
                snprintf(labels[i], sizeof(labels[i]), "%.60s (%ldB)", s_top[i].path, s_top[i].size);
            }
            items[i] = labels[i];
        }
        items[s_top_count] = "Back";

        char title[40];
        snprintf(title, sizeof(title), "CLEANUP %s used", header);
        int idx = k85_run_list_menu(title, items, s_top_count + 1, nullptr);
        if (idx < 0 || idx == s_top_count) return;

        delete_confirm(s_top[idx].path);
        // Дальше цикл сам пересканирует директорию заново - список актуализируется.
    }
}

#pragma GCC diagnostic pop
