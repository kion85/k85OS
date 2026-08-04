#include "music_player.h"
#include "common.h"
#include "theme.h"
#include "list_menu.h"
#include "power.h"
#include "input.h"
#include "log.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define K85_MP_MAX_ENTRIES 40

struct MpEntry {
    char name[64];
    bool is_dir;
};

static bool entry_is_wav(const char *name) {
    size_t len = strlen(name);
    return len > 4 && !strcasecmp(name + len - 4, ".wav");
}

static int list_dir_entries(const char *dir_path, MpEntry out[], int max_n) {
    DIR *d = opendir(dir_path);
    if (!d) return 0;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr && n < max_n) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        char full[300];
        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !entry_is_wav(ent->d_name)) continue;

        snprintf(out[n].name, sizeof(out[n].name), "%.60s", ent->d_name);
        out[n].is_dir = is_dir;
        n++;
    }
    closedir(d);

    std::sort(out, out + n, [](const MpEntry &a, const MpEntry &b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return strcmp(a.name, b.name) < 0;
    });
    return n;
}

static bool browse_for_wav(char *out_path, size_t out_size, const char *root) {
    char current[256];
    snprintf(current, sizeof(current), "%s", root);

    static MpEntry entries[K85_MP_MAX_ENTRIES];

    while (true) {
        int n = list_dir_entries(current, entries, K85_MP_MAX_ENTRIES);

        bool at_root = (strcmp(current, root) == 0);
        static char labels[K85_MP_MAX_ENTRIES][80];
        const char *items[K85_MP_MAX_ENTRIES + 2];

        int idx_offset = 0;
        if (!at_root) {
            items[idx_offset++] = "..";
        }
        for (int i = 0; i < n; i++) {
            snprintf(labels[i], sizeof(labels[i]), "%s%.60s",
                     entries[i].is_dir ? "[DIR] " : "", entries[i].name);
            items[idx_offset + i] = labels[i];
        }
        items[idx_offset + n] = "Back (exit)";

        int total = idx_offset + n + 1;
        int sel = k85_run_list_menu(current, items, total, nullptr);
        if (sel < 0 || sel == total - 1) return false;

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
            snprintf(out_path, out_size, "%s/%s", current, entries[local_idx].name);
            return true;
        }
    }
}

// ---------- Разбор WAV-заголовка (RIFF chunks) ----------
struct WavInfo {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    size_t data_offset;
    size_t data_size;
};

static bool parse_wav(const uint8_t *data, size_t size, WavInfo &info) {
    if (size < 44) return false;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return false;

    size_t pos = 12;
    bool have_fmt = false, have_data = false;

    while (pos + 8 <= size) {
        char chunk_id[5] = {0};
        memcpy(chunk_id, data + pos, 4);
        uint32_t chunk_size = 0;
        memcpy(&chunk_size, data + pos + 4, 4);
        size_t chunk_data = pos + 8;
        if (chunk_data > size) break;

        if (!memcmp(chunk_id, "fmt ", 4) && chunk_data + 16 <= size) {
            uint16_t channels = 0, bits_per_sample = 0;
            uint32_t sample_rate = 0;
            memcpy(&channels, data + chunk_data + 2, 2);
            memcpy(&sample_rate, data + chunk_data + 4, 4);
            memcpy(&bits_per_sample, data + chunk_data + 14, 2);
            info.channels = channels;
            info.sample_rate = sample_rate;
            info.bits_per_sample = bits_per_sample;
            have_fmt = true;
        } else if (!memcmp(chunk_id, "data", 4)) {
            info.data_offset = chunk_data;
            info.data_size = chunk_size;
            if (info.data_offset + info.data_size > size) {
                info.data_size = size - info.data_offset;
            }
            have_data = true;
        }

        size_t advance = chunk_size + (chunk_size % 2);
        pos = chunk_data + advance;
        if (have_fmt && have_data) break;
    }
    return have_fmt && have_data;
}

// ---------- Экран воспроизведения с иконками управления ----------
enum PlayerControl { CTRL_BACK10 = 0, CTRL_PLAYPAUSE = 1, CTRL_FWD10 = 2 };

static void draw_player_screen(const char *fname, const WavInfo &wav, size_t position,
                                bool playing, int selected) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int w = M5.Display.width();
    int h = M5.Display.height();

    M5.Display.fillScreen(bg);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(accent, bg);
    M5.Display.setCursor(6, 6);
    M5.Display.print("MUSIC PLAYER");

    M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(6, 20);
    M5.Display.printf("%.28s", fname);

    double bytes_per_sec = (double)wav.sample_rate * wav.channels * (wav.bits_per_sample / 8);
    if (bytes_per_sec <= 0) bytes_per_sec = 1;
    double cur_sec = position / bytes_per_sec;
    double total_sec = wav.data_size / bytes_per_sec;

    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02d:%02d / %02d:%02d",
             (int)cur_sec / 60, (int)cur_sec % 60, (int)total_sec / 60, (int)total_sec % 60);
    M5.Display.setCursor(6, 36);
    M5.Display.print(time_str);

    // Прогресс-бар
    int bar_x = 6, bar_y = 50, bar_w = w - 12, bar_h = 6;
    M5.Display.drawRect(bar_x, bar_y, bar_w, bar_h, 0x555555);
    float progress = total_sec > 0 ? (float)(cur_sec / total_sec) : 0;
    if (progress < 0) progress = 0;
    if (progress > 1) progress = 1;
    M5.Display.fillRect(bar_x + 1, bar_y + 1, (int)((bar_w - 2) * progress), bar_h - 2, accent);

    // Иконки управления: -10 | play/pause | +10
    int icon_y = h / 2 + 20;
    int cx = w / 2;
    int spacing = 50;

    // -10s (двойной левый треугольник)
    int bx = cx - spacing;
    uint32_t col_back = (selected == CTRL_BACK10) ? accent : fg;
    M5.Display.fillTriangle(bx, icon_y, bx + 10, icon_y - 8, bx + 10, icon_y + 8, col_back);
    M5.Display.fillTriangle(bx + 8, icon_y, bx + 18, icon_y - 8, bx + 18, icon_y + 8, col_back);
    M5.Display.setTextColor(col_back, bg);
    M5.Display.setCursor(bx - 8, icon_y + 14);
    M5.Display.print("-10s");

    // Play/Pause по центру
    uint32_t col_pp = (selected == CTRL_PLAYPAUSE) ? accent : fg;
    if (playing) {
        M5.Display.fillRect(cx - 8, icon_y - 8, 6, 16, col_pp);
        M5.Display.fillRect(cx + 2, icon_y - 8, 6, 16, col_pp);
    } else {
        M5.Display.fillTriangle(cx - 7, icon_y - 9, cx - 7, icon_y + 9, cx + 9, icon_y, col_pp);
    }

    // +10s (двойной правый треугольник)
    int fx = cx + spacing - 18;
    uint32_t col_fwd = (selected == CTRL_FWD10) ? accent : fg;
    M5.Display.fillTriangle(fx, icon_y - 8, fx, icon_y + 8, fx + 10, icon_y, col_fwd);
    M5.Display.fillTriangle(fx + 8, icon_y - 8, fx + 8, icon_y + 8, fx + 18, icon_y, col_fwd);
    M5.Display.setTextColor(col_fwd, bg);
    M5.Display.setCursor(fx - 2, icon_y + 14);
    M5.Display.print("+10s");

    M5.Display.setTextColor(0x777777, bg);
    M5.Display.setCursor(6, h - 12);
    M5.Display.print("A=select B=go  hold A+B=stop");
}

static void play_wav_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        k85_show_message("Playback not\nsupported\nA+B=back");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
        fclose(f);
        k85_show_message("Out of memory\nA+B=back");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }
    fread(data, 1, size, f);
    fclose(f);

    WavInfo wav = {};
    if (!parse_wav(data, (size_t)size, wav) || wav.bits_per_sample != 16) {
        free(data);
        k85_show_message("Unsupported WAV\n(need 16-bit PCM)\nA+B=back");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    double bytes_per_sec = (double)wav.sample_rate * wav.channels * (wav.bits_per_sample / 8);

    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;

    auto play_from = [&](size_t byte_pos) {
        size_t remaining = wav.data_size - byte_pos;
        size_t samples = remaining / sizeof(int16_t);
        const int16_t *ptr = (const int16_t *)(data + wav.data_offset + byte_pos);
        M5.Speaker.stop();
        M5.Speaker.playRaw(ptr, samples, wav.sample_rate, wav.channels == 2, 1);
    };

    size_t position = 0;
    size_t position_at_start = 0;
    int64_t play_start_us = esp_timer_get_time();
    bool playing = true;
    int selected = CTRL_PLAYPAUSE;

    play_from(0);
    draw_player_screen(fname, wav, position, playing, selected);

    uint32_t last_redraw = 0;
    while (true) {
        k85_input_update();

        if (k85_ab_held(600)) {
            k85_wait_ab_release();
            M5.Speaker.stop();
            free(data);
            return;
        }

        if (playing) {
            int64_t elapsed_us = esp_timer_get_time() - play_start_us;
            size_t elapsed_bytes = (size_t)((double)elapsed_us / 1000000.0 * bytes_per_sec);
            position = position_at_start + elapsed_bytes;
            if (position >= wav.data_size || !M5.Speaker.isPlaying()) {
                position = wav.data_size;
                playing = false;
            }
        }

        bool need_redraw = false;

        if (k85_btn_a_pressed()) {
            selected = (selected + 1) % 3;
            need_redraw = true;
        }

        if (k85_btn_b_pressed()) {
            if (selected == CTRL_PLAYPAUSE) {
                if (playing) {
                    M5.Speaker.stop();
                    playing = false;
                } else {
                    if (position >= wav.data_size) position = 0;
                    play_from(position);
                    position_at_start = position;
                    play_start_us = esp_timer_get_time();
                    playing = true;
                }
            } else {
                double delta_sec = (selected == CTRL_BACK10) ? -10.0 : 10.0;
                long new_pos = (long)position + (long)(delta_sec * bytes_per_sec);
                if (new_pos < 0) new_pos = 0;
                if ((size_t)new_pos > wav.data_size) new_pos = (long)wav.data_size;
                position = (size_t)new_pos;
                if (playing) {
                    play_from(position);
                    position_at_start = position;
                    play_start_us = esp_timer_get_time();
                }
            }
            need_redraw = true;
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (need_redraw || now - last_redraw > 300) {
            draw_player_screen(fname, wav, position, playing, selected);
            last_redraw = now;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_run_music_player(void) {
    while (true) {
        static const char *roots[] = {"/littlefs", "/sd", "Back"};
        int root_idx = k85_run_list_menu("MUSIC SOURCE", roots, 3, nullptr);
        if (root_idx < 0 || root_idx == 2) return;

        const char *root = roots[root_idx];

        DIR *test = opendir(root);
        if (!test) {
            k85_show_message("SD card not found\nA+B=back to menu");
            while (true) {
                k85_input_update();
                if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            continue; // назад к выбору источника, а не полный выход
        }
        closedir(test);

        char chosen_path[300];
        if (browse_for_wav(chosen_path, sizeof(chosen_path), root)) {
            play_wav_file(chosen_path);
        }
        // после плеера/отмены браузера — возвращаемся к выбору источника
    }
}

#pragma GCC diagnostic pop