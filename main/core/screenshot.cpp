#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "screenshot.h"
#include "config.h"
#include "svc_cfg.h"

#include "M5Unified.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef K85_SHOT_DIR
#define K85_SHOT_DIR "/littlefs/shots"
#endif
#define SHOT_MIN_FREE (160 * 1024)
#define SHOT_MAX_FILES 60

static const char *TAG = "k85_shot";

static bool s_readable_known = false;
static bool s_readable = false;
static bool s_gesture_cached = false;
static bool s_gesture_on = true;
static bool s_started = false;

static SemaphoreHandle_t shot_mtx(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }

static int next_shot_index(int *count) {
    int best = 0, n = 0;
    DIR *d = opendir(K85_SHOT_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            int v = 0;
            if (sscanf(e->d_name, "shot_%d.bmp", &v) == 1) { n++; if (v > best) best = v; }
        }
        closedir(d);
    }
    *count = n;
    return best + 1;
}

const char *k85_screenshot_result_text(K85ShotResult r) {
    switch (r) {
        case K85_SHOT_OK: return "saved";
        case K85_SHOT_NO_SPACE: return "not enough free space";
        case K85_SHOT_NO_MEM: return "out of memory";
        case K85_SHOT_IO: return "file error";
        case K85_SHOT_UNREADABLE: return "this display cannot be read back, screenshots are not supported";
        case K85_SHOT_BUSY: return "another screenshot is in progress";
        case K85_SHOT_TOO_MANY: return "too many screenshots (60), delete some in /shots";
    }
    return "error";
}

// Draws a 2x2 red and blue patch in the corner and checks that it reads back as red/blue.
// Restores the original 4 pixels (orig = 2 rows x 2 pixels x RGB) if the display is readable.
static bool probe_readable(const uint8_t orig[2][6]) {
    auto &d = M5.Display;
    uint8_t p1[12] = {0}, p2[12] = {0};
    d.fillRect(0, 0, 2, 2, (uint32_t)0xFF0000);
    d.readRectRGB(0, 0, 2, 2, p1);
    d.fillRect(0, 0, 2, 2, (uint32_t)0x0000FF);
    d.readRectRGB(0, 0, 2, 2, p2);
    bool red_ok = p1[0] > 200 && p1[1] < 60 && p1[2] < 60;
    bool blue_ok = p2[2] > 200 && p2[0] < 60 && p2[1] < 60;
    if (red_ok && blue_ok) {
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++) {
                const uint8_t *px = &orig[y][x * 3];
                d.drawPixel(x, y, (uint32_t)(((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2]));
            }
    }
    return red_ok && blue_ok;
}

K85ShotResult k85_screenshot_take(char *path_out, size_t path_size) {
    if (s_readable_known && !s_readable) return K85_SHOT_UNREADABLE;
    if (xSemaphoreTake(shot_mtx(), 0) != pdTRUE) return K85_SHOT_BUSY;

    K85ShotResult result = K85_SHOT_OK;
    uint8_t *row = nullptr;
    FILE *f = nullptr;
    char path[64] = {0};
    uint8_t orig[2][6];
    memset(orig, 0, sizeof(orig));

    const int w = (int)M5.Display.width(), h = (int)M5.Display.height();
    const int row_bytes = (w * 3 + 3) & ~3;
    const uint32_t img_bytes = (uint32_t)row_bytes * (uint32_t)h;

    size_t total = 0, used = 0;
    if (!k85_fs_info(&total, &used) || total < used || total - used < SHOT_MIN_FREE + img_bytes) { result = K85_SHOT_NO_SPACE; goto done; }
    mkdir(K85_SHOT_DIR, 0755);
    {
        int count = 0;
        int idx = next_shot_index(&count);
        if (count >= SHOT_MAX_FILES || idx > 9999) { result = K85_SHOT_TOO_MANY; goto done; }
        snprintf(path, sizeof(path), K85_SHOT_DIR "/shot_%04d.bmp", idx);
    }

    row = (uint8_t *)heap_caps_calloc(1, (size_t)row_bytes, MALLOC_CAP_SPIRAM);
    if (!row) row = (uint8_t *)calloc(1, (size_t)row_bytes);
    if (!row) { result = K85_SHOT_NO_MEM; goto done; }

    f = fopen(path, "wb");
    if (!f) { result = K85_SHOT_IO; goto done; }
    {
        uint8_t hd[54];
        memset(hd, 0, sizeof(hd));
        hd[0] = 'B'; hd[1] = 'M';
        le32(hd + 2, 54 + img_bytes);
        le32(hd + 10, 54);
        le32(hd + 14, 40);
        le32(hd + 18, (uint32_t)w);
        le32(hd + 22, (uint32_t)(-h));        // negative height = top-down rows
        le16(hd + 26, 1);
        le16(hd + 28, 24);
        le32(hd + 34, img_bytes);
        le32(hd + 38, 2835);
        le32(hd + 42, 2835);
        if (fwrite(hd, 1, 54, f) != 54) { result = K85_SHOT_IO; goto done; }
    }

    M5.Display.startWrite();                  // keep the UI from drawing in the middle of a row read
    for (int y = 0; y < h; y++) {
        memset(row, 0, (size_t)row_bytes);
        M5.Display.readRectRGB(0, y, w, 1, row);          // R,G,B per pixel
        if (y < 2 && w >= 2) memcpy(orig[y], row, 6);
        for (int x = 0; x < w; x++) { uint8_t t = row[x * 3]; row[x * 3] = row[x * 3 + 2]; row[x * 3 + 2] = t; }   // -> B,G,R
        if (fwrite(row, 1, (size_t)row_bytes, f) != (size_t)row_bytes) { result = K85_SHOT_IO; break; }
    }
    if (result == K85_SHOT_OK && !s_readable_known && w >= 2 && h >= 2) {
        s_readable = probe_readable(orig);
        s_readable_known = true;
        if (!s_readable) result = K85_SHOT_UNREADABLE;
    }
    M5.Display.endWrite();

done:
    if (f) { if (fclose(f) != 0 && result == K85_SHOT_OK) result = K85_SHOT_IO; }
    if (result != K85_SHOT_OK && path[0]) unlink(path);
    free(row);
    if (result == K85_SHOT_OK && path_out && path_size) snprintf(path_out, path_size, "%s", path);
    xSemaphoreGive(shot_mtx());
    return result;
}

// ---------- A+B tap gesture ----------
bool k85_screenshot_gesture_enabled(void) {
    if (!s_gesture_cached) {
        s_gesture_on = k85_svc_get_bool("screenshot", true);
        s_gesture_cached = true;
    }
    return s_gesture_on;
}

void k85_screenshot_set_gesture(bool on) {
    s_gesture_on = on;
    s_gesture_cached = true;
    k85_svc_set_bool("screenshot", on);
}

static void beep(int hz, int ms) {
    if (M5.Speaker.isEnabled() && !g_config.sound_muted && g_config.sound_volume > 0) M5.Speaker.tone((float)hz, (uint32_t)ms);
}

static void watcher_task(void *) {
    bool was_both = false, armed = false;
    int64_t t_start = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15));
        bool both = M5.BtnA.isPressed() && M5.BtnB.isPressed();
        int64_t now = esp_timer_get_time();
        if (both && !was_both) { t_start = now; armed = true; }
        if (both && armed && now - t_start >= 450000) armed = false;        // a hold is the exit gesture
        if (!both && was_both) {
            int64_t dur = now - t_start;
            if (armed && dur >= 30000 && dur < 450000 && k85_screenshot_gesture_enabled()) {
                char path[64];
                K85ShotResult r = k85_screenshot_take(path, sizeof(path));
                if (r == K85_SHOT_OK) {
                    ESP_LOGI(TAG, "saved %s", path);
                    beep(2200, 40);
                } else {
                    ESP_LOGW(TAG, "screenshot failed: %s", k85_screenshot_result_text(r));
                    beep(300, 120);
                }
            }
            armed = false;
        }
        was_both = both;
    }
}

void k85_screenshot_start(void) {
    if (s_started) return;
    s_started = true;
    xTaskCreate(watcher_task, "k85_shot", 6144, nullptr, 2, nullptr);
}

#pragma GCC diagnostic pop
