#include "files.h"
#include "common.h"
#include "text_input.h"
#include "list_menu.h"
#include "log.h"
#include "input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../core/theme.h"
#include "../../core/bios_theme.h"
#include "../../core/config.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define K85_CUSTOM_THEME_MAX_HINT 8

static void browse_dir(const char *root) {
    char lines_buf[16][64];
    const char *lines[16];
    int n = 0;

    DIR *d = opendir(root);
    if (!d) {
        snprintf(lines_buf[0], 64, "%s - not mounted", root);
        lines[0] = lines_buf[0];
        k85_area_show(lines, 1, "FILES");
        return;
    }

    snprintf(lines_buf[n], 64, "%s", root);
    lines[n] = lines_buf[n]; n++;

    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr && n < 16) {
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", root, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            bool is_dir = S_ISDIR(st.st_mode);
            snprintf(lines_buf[n], 64, "%s%.40s %ldB", is_dir ? "[DIR] " : "      ",
                     ent->d_name, (long)st.st_size);
        } else {
            snprintf(lines_buf[n], 64, "      %.50s", ent->d_name);
        }
        lines[n] = lines_buf[n]; n++;
    }
    closedir(d);
    k85_area_show(lines, n, "FILES");
}

// TODO: без statvfs (его нет в toolchain) считаем общий объём вручную обходом
// файлов первого уровня — это НЕ равно реальной свободной ёмкости тома, только
// сумма размеров видимых файлов. Настоящий total/free можно получить только зная,
// какой ФС-драйвер монтирует /sd (esp_vfs_fat_sdmmc_mount даёт f_getfree() из FATFS
// напрямую) — скажи, чем у тебя смонтирована карта, доделаю точно.
static void sd_info(void) {
    char lines_buf[8][48];
    const char *lines[8];
    int n = 0;

    DIR *d = opendir("/sd");
    if (!d) {
        snprintf(lines_buf[n], 48, "SD card not found!"); lines[n] = lines_buf[n]; n++;
        k85_area_show(lines, n, "SD CARD");
        return;
    }

    long total_bytes = 0;
    int file_count = 0;
    struct dirent *ent;
    char names[8][40];
    int shown = 0;
    while ((ent = readdir(d)) != nullptr) {
        char full[256];
        snprintf(full, sizeof(full), "/sd/%s", ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            total_bytes += st.st_size;
            file_count++;
            if (shown < 8) { snprintf(names[shown], 40, "%s", ent->d_name); shown++; }
        }
    }
    closedir(d);

    snprintf(lines_buf[n], 48, "Files: %d", file_count); lines[n] = lines_buf[n]; n++;
    snprintf(lines_buf[n], 48, "Total size: %.1f MB", total_bytes / (1024.0 * 1024.0));
    lines[n] = lines_buf[n]; n++;
    for (int i = 0; i < shown && n < 8; i++) {
        snprintf(lines_buf[n], 48, " %.40s", names[i]);
        lines[n] = lines_buf[n]; n++;
    }
    k85_area_show(lines, n, "SD CARD");
}

static void copy_sd_to_flash(void) {
    DIR *d = opendir("/sd");
    if (!d) {
        k85_show_message("No SD card\nA+B=back");
        return;
    }
    char names[16][64];
    const char *items[17];
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr && n < 16) {
        struct stat st;
        char full[256];
        snprintf(full, sizeof(full), "/sd/%s", ent->d_name);
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(names[n], 64, "%.60s", ent->d_name);
            n++;
        }
    }
    closedir(d);
    if (n == 0) {
        k85_show_message("No files on SD\nA+B=back");
        return;
    }
    for (int i = 0; i < n; i++) items[i] = names[i];
    items[n] = "Back";

    int idx = k85_run_list_menu("Copy from SD", items, n + 1, nullptr);
    if (idx < 0 || idx >= n) return;

    char src[280], dst[280];
    snprintf(src, sizeof(src), "/sd/%s", names[idx]);
    snprintf(dst, sizeof(dst), "/littlefs/%s", names[idx]);

    FILE *fsrc = fopen(src, "rb");
    if (!fsrc) { k85_show_message("Copy error:\nopen src"); return; }
    FILE *fdst = fopen(dst, "wb");
    if (!fdst) { fclose(fsrc); k85_show_message("Copy error:\nopen dst"); return; }

    char buf[512];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        fwrite(buf, 1, r, fdst);
    }
    fclose(fsrc);
    fclose(fdst);
    k85_log("Copied %s -> %s", src, dst);

    char msg[96];
    snprintf(msg, sizeof(msg), "Copied!\n%.60s", names[idx]);
    k85_show_message(msg);
}

static void apply_theme_picker(void) {
    k85_themes_load_custom(); // на случай, если файл только что загрузили

    int total = k85_theme_count();
    int custom_n = total - K85_THEME_COUNT;
    if (custom_n <= 0) {
        k85_show_message("No .thm files\nin /littlefs\nA+B=back");
        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        return;
    }

    const char *names[K85_CUSTOM_THEME_MAX_HINT];
    for (int i = 0; i < custom_n; i++) {
        names[i] = k85_get_theme_by_index(K85_THEME_COUNT + i)->name;
    }

    int idx = k85_run_list_menu("APPLY THEME", names, custom_n, nullptr);
    if (idx < 0) return;

    g_config.theme_idx = K85_THEME_COUNT + idx;
    k85_config_save();

    char msg[64];
    snprintf(msg, sizeof(msg), "Applied: %.40s\nA+B=back", names[idx]);
    k85_show_message(msg);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void apply_bios_theme_picker(void) {
    mkdir("/littlefs/bios", 0755); // на случай, если папки ещё нет

    DIR *d = opendir("/littlefs/bios");
    if (!d) return;

    char names[16][40];
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr && n < 16) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && !strcasecmp(ent->d_name + len - 4, ".thm")) {
            snprintf(names[n], sizeof(names[n]), "%.35s", ent->d_name);
            n++;
        }
    }
    closedir(d);

    if (n == 0) {
        k85_show_message("No bios_*.thm files\nin /littlefs\nA+B=back");
        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        return;
    }

    const char *items[17];
    for (int i = 0; i < n; i++) items[i] = names[i];
    items[n] = "Back";

    int idx = k85_run_list_menu("APPLY BIOS THEME", items, n + 1, nullptr);
    if (idx < 0 || idx >= n) return;

    char src[192];
    snprintf(src, sizeof(src), "/littlefs/bios/%s", names[idx]);

    FILE *fsrc = fopen(src, "rb");
    FILE *fdst = fopen(K85_BIOS_THEME_ACTIVE_FILE, "wb");
    if (fsrc && fdst) {
        char buf[256];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), fsrc)) > 0) fwrite(buf, 1, r, fdst);
    }
    if (fsrc) fclose(fsrc);
    if (fdst) fclose(fdst);

    char msg[64];
    snprintf(msg, sizeof(msg), "BIOS theme applied:\n%.35s\nA+B=back", names[idx]);
    k85_show_message(msg);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_run_files(void) {
    const char *items[] = {"Browse /littlefs", "Browse /sd", "SD Card Info", "Copy SD->Flash", "Apply theme", "Apply BIOS theme", "Back"};
    while (true) {
        int idx = k85_run_list_menu("FILES", items, 7, nullptr);
        if (idx < 0 || idx == 6) return;
        if (idx == 0) browse_dir("/littlefs");
        else if (idx == 1) browse_dir("/sd");
        else if (idx == 2) sd_info();
        else if (idx == 3) copy_sd_to_flash();
        else if (idx == 4) apply_theme_picker();
        else if (idx == 5) apply_bios_theme_picker();
    }
}

#pragma GCC diagnostic pop









