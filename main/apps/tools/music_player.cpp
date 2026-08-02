#include "music_player.h"
#include "common.h"
#include "list_menu.h"
#include "power.h"
#include "input.h"
#include "log.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

static void collect_wavs(const char *dir, char tracks[][256], int &n, int max_n) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr && n < max_n) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && !strcasecmp(ent->d_name + len - 4, ".wav")) {
            snprintf(tracks[n], 256, "%s/%s", dir, ent->d_name);
            n++;
        }
    }
    closedir(d);
}

void k85_run_music_player(void) {
    static char tracks[16][256];
    int n = 0;
    collect_wavs("/littlefs/music", tracks, n, 16);
    collect_wavs("/sd/music", tracks, n, 16);
    collect_wavs("/sd", tracks, n, 16);

    if (n == 0) {
        k85_show_message("No .wav files\nin /littlefs/music\nor /sd/music");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    const char *items[17];
    for (int i = 0; i < n; i++) items[i] = tracks[i];
    items[n] = "Back";

    int idx = k85_run_list_menu("MUSIC", items, n + 1, nullptr);
    if (idx < 0 || idx >= n) return;

    FILE *f = fopen(tracks[idx], "rb");
    if (!f) {
        k85_show_message("Playback not\nsupported");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // TODO: файл грузится целиком в кучу (malloc) — для больших .wav может не хватить
    // памяти; для потокового воспроизведения нужен отдельный слой с чтением по кускам.
    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
        fclose(f);
        k85_show_message("Out of memory");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }
    fread(data, 1, size, f);
    fclose(f);

    const char *fname = strrchr(tracks[idx], '/');
    fname = fname ? fname + 1 : tracks[idx];
    char msg[280];
    snprintf(msg, sizeof(msg), "Playing:\n%.60s\nA+B=stop", fname);
    k85_show_message(msg);

    M5.Speaker.playWav(data, (size_t)size);

    while (true) {
        k85_input_update();
        if (k85_ab_held(400)) {
            k85_wait_ab_release();
            break;
        }
        if (!M5.Speaker.isPlaying()) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    M5.Speaker.stop();
    free(data);
}

#pragma GCC diagnostic pop


