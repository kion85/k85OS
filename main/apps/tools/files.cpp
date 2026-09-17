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
#include "../../core/boot_theme.h"
#include "../../core/config.h"
#include "music_player.h"

#include "M5Unified.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define K85_CUSTOM_THEME_MAX_HINT 8

#define K85_FB_MAX_ENTRIES 40
#define K85_FILE_EDIT_MAX_BYTES 2047 // должно совпадать с буфером в k85_text_input_multiline

struct FbEntry {
    char name[64];
    bool is_dir;
    long size;
};

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Read: постраничный просмотр содержимого файла ----------
static void read_file_view(const char *full_path, const char *name, long size) {
    FILE *f = fopen(full_path, "rb");
    if (!f) {
        k85_show_message("Open failed\nA+B=back");
        wait_ab_exit();
        return;
    }
    static char buf[4096];
    size_t read_n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[read_n] = 0;
    bool truncated = (size > (long)(sizeof(buf) - 1));

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int w = M5.Display.width();
    int h = M5.Display.height();
    int chars_per_line = w / 6;
    int lines_per_page = (h - 26) / 10;
    int chars_per_page = chars_per_line * lines_per_page;
    if (chars_per_page < 16) chars_per_page = 16;

    int page_count = (int)((read_n + (size_t)chars_per_page - 1) / (size_t)chars_per_page);
    if (page_count < 1) page_count = 1;
    int page = 0;

    while (true) {
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.printf("%.16s  %ldB  [%d/%d]", name, size, page + 1, page_count);

        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, 14);
        M5.Display.setTextWrap(true, false);
        size_t start = (size_t)page * (size_t)chars_per_page;
        size_t remain = read_n > start ? read_n - start : 0;
        size_t show_len = remain < (size_t)chars_per_page ? remain : (size_t)chars_per_page;
        M5.Display.printf("%.*s", (int)show_len, buf + start);
        M5.Display.setTextWrap(false, false);

        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, h - 12);
        if (truncated && page == page_count - 1) {
            M5.Display.print("A=next A+B=back (truncated)");
        } else {
            M5.Display.print("A=next page  A+B=back");
        }

        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
            if (k85_btn_a_pressed()) { page = (page + 1) % page_count; break; }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}

// ---------- Write: редактирование файла через многострочную клавиатуру ----------
static void write_file_edit(const char *full_path, const char *name) {
    static char content[K85_FILE_EDIT_MAX_BYTES + 1];
    content[0] = 0;

    FILE *f = fopen(full_path, "rb");
    if (f) {
        size_t n = fread(content, 1, sizeof(content) - 1, f);
        content[n] = 0;
        fclose(f);
    }

    static char edited[K85_FILE_EDIT_MAX_BYTES + 1];
    if (!k85_text_input_multiline(name, content, edited, sizeof(edited))) {
        return; // EXIT - изменения отброшены
    }

    FILE *fw = fopen(full_path, "wb");
    if (!fw) {
        k85_show_message("Write failed\nA+B=back");
        wait_ab_exit();
        return;
    }
    fwrite(edited, 1, strlen(edited), fw);
    fclose(fw);

    k85_show_message("Saved!\nA+B=back");
    wait_ab_exit();
}

// ---------- Delete ----------
// Возвращает true, если файл реально удалён (чтобы вызывающий код мог
// закрыть меню действий и обновить список).
static bool delete_file_confirm(const char *full_path, const char *name) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Delete %.40s?\nB=confirm A+B=cancel", name);
    k85_show_message(msg);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    bool ok = (remove(full_path) == 0);
    k85_show_message(ok ? "Deleted!\nA+B=back" : "Delete failed\nA+B=back");
    wait_ab_exit();
    return ok;
}

// ---------- Run: действие зависит от расширения файла ----------
// Пока поддержаны только .thm (применяется как обычная тема устройства -
// та же логика, что в apply_theme_picker, но для конкретного выбранного
// файла вместо выбора из общего списка). Для остальных типов - заглушка.
static void run_file(const char *full_path, const char *name) {
    size_t len = strlen(name);
    bool is_thm = (len > 4 && !strcasecmp(name + len - 4, ".thm"));
    bool is_mp3 = (len > 4 && !strcasecmp(name + len - 4, ".mp3"));
    bool is_wav = (len > 4 && !strcasecmp(name + len - 4, ".wav"));
    if (is_mp3 || is_wav) {
        k85_play_audio_file(full_path);
        return;
    }

    if (!is_thm) {
        k85_show_message("Run: not supported\nfor this file type\nA+B=back");
        wait_ab_exit();
        return;
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "Apply %.40s\nas active theme?\nB=confirm A+B=cancel", name);
    k85_show_message(msg);
    bool confirmed = false;
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_b_pressed()) { confirmed = true; break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (!confirmed) return;

    // Копируем выбранный .thm в /littlefs (папку с пользовательскими темами),
    // затем перезагружаем список тем и применяем именно этот файл.
    char dst[192];
    snprintf(dst, sizeof(dst), "/littlefs/%s", name);

    if (strcmp(full_path, dst) != 0) {
        FILE *fsrc = fopen(full_path, "rb");
        FILE *fdst = fopen(dst, "wb");
        if (fsrc && fdst) {
            char buf[256];
            size_t r;
            while ((r = fread(buf, 1, sizeof(buf), fsrc)) > 0) fwrite(buf, 1, r, fdst);
        }
        if (fsrc) fclose(fsrc);
        if (fdst) fclose(fdst);
    }

    k85_themes_load_custom();
    int total = k85_theme_count();
    int applied_idx = -1;
    for (int i = K85_THEME_COUNT; i < total; i++) {
        if (!strcmp(k85_get_theme_by_index(i)->name, name)) { applied_idx = i; break; }
    }
    if (applied_idx < 0) {
        k85_show_message("Apply failed\n(invalid theme file?)\nA+B=back");
        wait_ab_exit();
        return;
    }

    g_config.theme_idx = applied_idx;
    k85_config_save();
    k85_show_message("Theme applied!\nA+B=back");
    wait_ab_exit();
}

static void view_edit_menu(const char *full_path, const char *name, long size) {
    static const char *items[] = {"Read", "Write", "Back"};
    while (true) {
        int idx = k85_run_list_menu(name, items, 3, nullptr);
        if (idx < 0 || idx == 2) return;
        if (idx == 0) read_file_view(full_path, name, size);
        else if (idx == 1) write_file_edit(full_path, name);
    }
}

// Возвращает true, если файл был удалён (вызывающий код должен обновить список).
// Возвращает true, если файл переименован (путь изменился - вызывающий
// код должен выйти из меню действий, чтобы список обновился под новым именем).
static bool rename_file_confirm(const char *full_path, const char *name) {
    char new_name[64] = "";
    snprintf(new_name, sizeof(new_name), "%s", name);
    if (!k85_text_input("New name:", new_name, new_name, sizeof(new_name)) || !new_name[0]) return false;
    if (!strcmp(new_name, name)) return false; // имя не изменилось

    char dir[256];
    snprintf(dir, sizeof(dir), "%s", full_path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) *last_slash = 0;

    char new_path[300];
    snprintf(new_path, sizeof(new_path), "%s/%s", dir, new_name);

    bool ok = (rename(full_path, new_path) == 0);
    k85_show_message(ok ? "Renamed!\nA+B=back" : "Rename failed\nA+B=back");
    wait_ab_exit();
    return ok;
}

static bool file_action_menu(const char *full_path, const char *name, long size) {
    static const char *items[] = {"View/Edit", "Run", "Rename", "Delete", "Back"};
    while (true) {
        int idx = k85_run_list_menu(name, items, 5, nullptr);
        if (idx < 0 || idx == 4) return false;
        if (idx == 0) {
            view_edit_menu(full_path, name, size);
        } else if (idx == 1) {
            run_file(full_path, name);
        } else if (idx == 2) {
            if (rename_file_confirm(full_path, name)) return true;
        } else if (idx == 3) {
            if (delete_file_confirm(full_path, name)) return true;
        }
    }
}

static int fb_list_entries(const char *dir_path, FbEntry out[], int max_n) {
    DIR *d = opendir(dir_path);
    if (!d) return -1;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr && n < max_n) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char full[300];
        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        snprintf(out[n].name, sizeof(out[n].name), "%.60s", ent->d_name);
        out[n].is_dir = S_ISDIR(st.st_mode);
        out[n].size = out[n].is_dir ? 0 : (long)st.st_size;
        n++;
    }
    closedir(d);
    return n;
}

// Полноценный браузер: заходим в папки, поднимаемся через "..", видим размеры файлов.
static void browse_dir(const char *root) {
    char current[256];
    snprintf(current, sizeof(current), "%s", root);

    static FbEntry entries[K85_FB_MAX_ENTRIES];

    while (true) {
        int n = fb_list_entries(current, entries, K85_FB_MAX_ENTRIES);
        if (n < 0) {
            char msg[80];
            snprintf(msg, sizeof(msg), "%s - not mounted\nA+B=back", root);
            k85_show_message(msg);
            while (true) {
                k85_input_update();
                if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            return;
        }

        bool at_root = (strcmp(current, root) == 0);
        static char labels[K85_FB_MAX_ENTRIES][64];
        const char *items[K85_FB_MAX_ENTRIES + 2];

        int idx_offset = 0;
        if (!at_root) items[idx_offset++] = "..";

        for (int i = 0; i < n; i++) {
            if (entries[i].is_dir) {
                snprintf(labels[i], sizeof(labels[i]), "[DIR] %.44s", entries[i].name);
            } else {
                snprintf(labels[i], sizeof(labels[i]), "%.40s (%ldB)", entries[i].name, entries[i].size);
            }
            items[idx_offset + i] = labels[i];
        }
        items[idx_offset + n] = "Back (exit)";

        int total = idx_offset + n + 1;
        int sel = k85_run_list_menu(current, items, total, nullptr);
        if (sel < 0 || sel == total - 1) return;

        if (!at_root && sel == 0) {
            char *last_slash = strrchr(current, '/');
            if (last_slash && last_slash != current) *last_slash = 0;
            continue;
        }

        int local_idx = sel - idx_offset;
        if (local_idx < 0 || local_idx >= n) continue;

        if (entries[local_idx].is_dir) {
            char next[300];
            snprintf(next, sizeof(next), "%s/%s", current, entries[local_idx].name);
            snprintf(current, sizeof(current), "%s", next);
        } else {
            char full_path[300];
            snprintf(full_path, sizeof(full_path), "%s/%s", current, entries[local_idx].name);
            file_action_menu(full_path, entries[local_idx].name, entries[local_idx].size);
            // Список entries[] мог устареть (файл переименован/удалён/изменён) -
            // внешний while(true) сам перечитает директорию на следующей итерации.
        }
    }
}

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
    k85_themes_load_custom();

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
    mkdir("/littlefs/bios", 0755);

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
        k85_show_message("No .thm files in\n/littlefs/bios\nA+B=back");
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

    g_config.bios_bg_color = 0xFFFFFFFF;
    g_config.bios_hl_color = 0xFFFFFFFF;
    g_config.bios_text_color = 0xFFFFFFFF;
    k85_config_save();

    char msg[64];
    snprintf(msg, sizeof(msg), "BIOS theme applied:\n%.35s\nA+B=back", names[idx]);
    k85_show_message(msg);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void apply_grub_theme_picker(void) {
    mkdir("/littlefs/grub", 0755);

    DIR *d = opendir("/littlefs/grub");
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
        k85_show_message("No .thm files in\n/littlefs/grub\nA+B=back");
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

    int idx = k85_run_list_menu("APPLY GRUB THEME", items, n + 1, nullptr);
    if (idx < 0 || idx >= n) return;

    char src[192];
    snprintf(src, sizeof(src), "/littlefs/grub/%s", names[idx]);

    FILE *fsrc = fopen(src, "rb");
    FILE *fdst = fopen(K85_BOOT_THEME_ACTIVE_FILE, "wb");
    if (fsrc && fdst) {
        char buf[256];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), fsrc)) > 0) fwrite(buf, 1, r, fdst);
    }
    if (fsrc) fclose(fsrc);
    if (fdst) fclose(fdst);

    char msg[64];
    snprintf(msg, sizeof(msg), "GRUB theme applied:\n%.35s\nA+B=back", names[idx]);
    k85_show_message(msg);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_run_files(void) {
    const char *items[] = {"Browse /littlefs", "Browse /sd", "SD Card Info", "Copy SD->Flash", "Apply theme", "Apply BIOS theme", "Apply GRUB theme", "Back"};
    while (true) {
        int idx = k85_run_list_menu("FILES", items, 8, nullptr);
        if (idx < 0 || idx == 7) return;
        if (idx == 0) browse_dir("/littlefs");
        else if (idx == 1) browse_dir("/sd");
        else if (idx == 2) sd_info();
        else if (idx == 3) copy_sd_to_flash();
        else if (idx == 4) apply_theme_picker();
        else if (idx == 5) apply_bios_theme_picker();
        else if (idx == 6) apply_grub_theme_picker();
    }
}

#pragma GCC diagnostic pop
