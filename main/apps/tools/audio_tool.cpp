#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "audio_tool.h"
#include "common.h"
#include "input.h"
#include "config.h"
#include "list_menu.h"
#include "sound.h"
#include "theme.h"

#include "M5Unified.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUD_RATE        16000
#define AUD_FFT_N       512
#define AUD_BUF_N       1024
#define AUD_BANDS       24
#define AUD_REC_DIR     "/littlefs/rec"
#define AUD_MAX_REC_SEC 120
#define AUD_MIN_FREE    (192 * 1024)
#define AUD_STOP_FREE   (64 * 1024)
#define AUD_MAX_FILES   24
#define AUD_BAND_FLOOR  (-90.0f)   // dBFS shown as an empty bar
#define AUD_BAND_SPAN   70.0f      // up to -20 dBFS = full bar
#define AUD_VU_FLOOR    (-60.0f)   // level meter: -60 .. 0 dBFS

// ---------- small helpers ----------
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

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

static void wav_header(uint8_t h[44], uint32_t data_bytes) {
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);               // PCM
    le16(h + 22, 1);               // mono
    le32(h + 24, AUD_RATE);
    le32(h + 28, AUD_RATE * 2);
    le16(h + 32, 2);
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, data_bytes);
}

static char *psram_alloc(size_t n) {
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    if (!p) p = calloc(1, n);
    return (char *)p;
}

// 0 = green, 0.5 = yellow, 1 = red
static uint32_t heat(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int r = (int)(255.0f * fminf(1.0f, 2.0f * t));
    int g = (int)(255.0f * fminf(1.0f, 2.0f * (1.0f - t)));
    return ((uint32_t)r << 16) | ((uint32_t)g << 8);
}

static float to_db(float amp) { return 20.0f * log10f(amp / 32768.0f + 1e-9f); }

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ---------- microphone / speaker switching ----------
// On some M5 boards the speaker and microphone share one I2S port, so one must be
// released before the other is started. Harmless on boards with separate ports.
static bool mic_start(void) {
    if (!M5.Mic.isEnabled()) return false;
    M5.Speaker.stop();
    M5.Speaker.end();
    if (!M5.Mic.begin()) {
        M5.Speaker.begin();
        k85_apply_sound_volume();
        return false;
    }
    return true;
}

static void mic_stop(void) {
    int guard = 0;
    while (M5.Mic.isRecording() && guard++ < 500) vTaskDelay(1);
    M5.Mic.end();
    M5.Speaker.begin();
    k85_apply_sound_volume();
}

static bool mic_submit(int16_t *buf, int n) {
    for (int tries = 0; tries < 300; tries++) {
        if (M5.Mic.record(buf, n, AUD_RATE)) return true;
        vTaskDelay(1);
    }
    return false;
}

// ---------- audio context (PSRAM) ----------
struct Audio {
    int16_t buf[2][AUD_BUF_N];
    float re[AUD_FFT_N], im[AUD_FFT_N], win[AUD_FFT_N];
    float cs[AUD_FFT_N / 2], sn[AUD_FFT_N / 2];
    int edge[AUD_BANDS + 1];
    float bar[AUD_BANDS], peak[AUD_BANDS];
    int hold[AUD_BANDS];
};

static Audio *audio_alloc(void) {
    Audio *a = (Audio *)psram_alloc(sizeof(Audio));
    if (!a) return nullptr;
    const float two_pi = 6.2831853f;
    for (int i = 0; i < AUD_FFT_N; i++) a->win[i] = 0.5f - 0.5f * cosf(two_pi * (float)i / (float)(AUD_FFT_N - 1));
    for (int k = 0; k < AUD_FFT_N / 2; k++) {
        a->cs[k] = cosf(two_pi * (float)k / (float)AUD_FFT_N);
        a->sn[k] = -sinf(two_pi * (float)k / (float)AUD_FFT_N);
    }
    // log-spaced bands from bin 4 (125 Hz) to bin 256 (8 kHz)
    for (int i = 0; i <= AUD_BANDS; i++) a->edge[i] = (int)(4.0f * powf(64.0f, (float)i / (float)AUD_BANDS) + 0.5f);
    for (int i = 1; i <= AUD_BANDS; i++) if (a->edge[i] <= a->edge[i - 1]) a->edge[i] = a->edge[i - 1] + 1;
    return a;
}

static void fft(Audio *a) {
    const int n = AUD_FFT_N;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = a->re[i]; a->re[i] = a->re[j]; a->re[j] = t;
            t = a->im[i]; a->im[i] = a->im[j]; a->im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = a->cs[k * step], wi = a->sn[k * step];
                int u = i + k, v = i + k + half;
                float tr = a->re[v] * wr - a->im[v] * wi;
                float ti = a->re[v] * wi + a->im[v] * wr;
                a->re[v] = a->re[u] - tr;
                a->im[v] = a->im[u] - ti;
                a->re[u] += tr;
                a->im[u] += ti;
            }
        }
    }
}

// rms_db: level of the whole frame in dBFS
static float frame_rms_db(const int16_t *s, int n) {
    double mean = 0;
    for (int i = 0; i < n; i++) mean += s[i];
    mean /= n;
    double sum = 0;
    for (int i = 0; i < n; i++) { double v = s[i] - mean; sum += v * v; }
    return to_db((float)sqrt(sum / n));
}

// Updates a->bar / a->peak from the last AUD_FFT_N samples of the frame.
static void analyze(Audio *a, const int16_t *s, int n, float gain_db) {
    const int16_t *t = s + (n - AUD_FFT_N);
    float mean = 0;
    for (int i = 0; i < AUD_FFT_N; i++) mean += t[i];
    mean /= AUD_FFT_N;
    for (int i = 0; i < AUD_FFT_N; i++) { a->re[i] = ((float)t[i] - mean) * a->win[i]; a->im[i] = 0.0f; }
    fft(a);

    const float norm = 4.0f / (float)AUD_FFT_N;   // Hann window: peak = A * N / 4
    for (int b = 0; b < AUD_BANDS; b++) {
        int lo = a->edge[b], hi = a->edge[b + 1];
        if (hi > AUD_FFT_N / 2) hi = AUD_FFT_N / 2;
        if (lo >= hi) lo = hi - 1;
        float m = 0;
        for (int k = lo; k < hi; k++) {
            float p = a->re[k] * a->re[k] + a->im[k] * a->im[k];
            if (p > m) m = p;
        }
        float db = to_db(sqrtf(m) * norm) + gain_db;
        float lvl = clamp01((db - AUD_BAND_FLOOR) / AUD_BAND_SPAN);

        if (lvl >= a->bar[b]) a->bar[b] = lvl;
        else { a->bar[b] -= 0.07f; if (a->bar[b] < lvl) a->bar[b] = lvl; }

        if (a->bar[b] >= a->peak[b]) { a->peak[b] = a->bar[b]; a->hold[b] = 14; }
        else if (a->hold[b] > 0) a->hold[b]--;
        else { a->peak[b] -= 0.025f; if (a->peak[b] < a->bar[b]) a->peak[b] = a->bar[b]; }
    }
}

// ---------- drawing ----------
struct Layout { int w, h, sp_x, sp_y, sp_w, sp_h, lab_y, vu_x, vu_y, vu_w, vu_h, tick_y, txt_y; };

static Layout make_layout(void) {
    Layout L;
    L.w = M5.Display.width();
    L.h = M5.Display.height();
    L.sp_x = 4;
    L.sp_w = ((L.w - 8) / AUD_BANDS) * AUD_BANDS;
    L.sp_y = 14;
    L.sp_h = L.h * 48 / 100;
    L.lab_y = L.sp_y + L.sp_h + 2;
    L.vu_x = 4;
    L.vu_w = L.w - 8;
    L.vu_y = L.lab_y + 14;
    L.vu_h = 10;
    L.tick_y = L.vu_y + L.vu_h + 2;
    L.txt_y = L.tick_y + 10;
    return L;
}

static void put_text(int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(x, y);
    M5.Display.print(s);
}

static void put_padded(int x, int y, const char *s, int width_chars, uint32_t fg, uint32_t bg) {
    char pad[64];
    snprintf(pad, sizeof(pad), "%-*.*s", width_chars, width_chars, s);
    put_text(x, y, pad, fg, bg);
}

static void draw_static(const Layout &L, uint32_t bg, uint32_t fg, bool with_spectrum) {
    auto &d = M5.Display;
    d.fillScreen(bg);
    d.setTextSize(1);

    if (with_spectrum) {
        static const char *const names[4] = { "125", "500", "2k", "8k" };
        for (int i = 0; i < 4; i++) {
            int tw = (int)strlen(names[i]) * 6;
            int x = L.sp_x + (int)((float)i / 3.0f * (float)(L.sp_w - tw));
            put_text(x, L.lab_y, names[i], fg, bg);
        }
        d.drawFastHLine(L.sp_x, L.sp_y + L.sp_h, L.sp_w, fg);
    }

    d.drawRect(L.vu_x - 1, L.vu_y - 1, L.vu_w + 2, L.vu_h + 2, fg);
    static const int ticks[4] = { -60, -40, -20, 0 };
    for (int i = 0; i < 4; i++) {
        int x = L.vu_x + (int)((float)(ticks[i] + 60) / 60.0f * (float)(L.vu_w - 1));
        d.drawFastVLine(x, L.vu_y + L.vu_h + 1, 3, fg);
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d", ticks[i]);
        int tw = (int)strlen(lbl) * 6;
        int tx = x - tw / 2;
        if (tx < 0) tx = 0;
        if (tx > L.w - tw) tx = L.w - tw;
        put_text(tx, L.vu_y + L.vu_h + 4, lbl, fg, bg);
    }
}

static void draw_bands(const Audio *a, const Layout &L, uint32_t bg) {
    auto &d = M5.Display;
    int bw = L.sp_w / AUD_BANDS;
    for (int b = 0; b < AUD_BANDS; b++) {
        int x = L.sp_x + b * bw;
        int hpx = (int)(a->bar[b] * (float)L.sp_h);
        int top = L.sp_y + L.sp_h - hpx;
        if (top > L.sp_y) d.fillRect(x, L.sp_y, bw - 2, top - L.sp_y, bg);
        if (hpx > 0) d.fillRect(x, top, bw - 2, hpx, heat(a->bar[b]));
        if (a->peak[b] > a->bar[b] + 0.02f) {
            int pp = L.sp_y + L.sp_h - (int)(a->peak[b] * (float)L.sp_h);
            if (pp >= L.sp_y) d.fillRect(x, pp, bw - 2, 2, (uint32_t)0xFFFFFF);
        }
    }
}

// frac / peak: 0..1 on the -60..0 dBFS scale
static void draw_vu(const Layout &L, float frac, float peak) {
    auto &d = M5.Display;
    int fill = (int)(clamp01(frac) * (float)L.vu_w);
    int g_end = L.vu_w * 60 / 100;
    int y_end = L.vu_w * 85 / 100;
    d.fillRect(L.vu_x, L.vu_y, L.vu_w, L.vu_h, (uint32_t)0x202020);
    int g = fill < g_end ? fill : g_end;
    if (g > 0) d.fillRect(L.vu_x, L.vu_y, g, L.vu_h, (uint32_t)0x00C000);
    if (fill > g_end) {
        int y = fill < y_end ? fill : y_end;
        d.fillRect(L.vu_x + g_end, L.vu_y, y - g_end, L.vu_h, (uint32_t)0xFFD000);
    }
    if (fill > y_end) d.fillRect(L.vu_x + y_end, L.vu_y, fill - y_end, L.vu_h, (uint32_t)0xFF2000);
    int px = L.vu_x + (int)(clamp01(peak) * (float)(L.vu_w - 2));
    d.fillRect(px, L.vu_y, 2, L.vu_h, (uint32_t)0xFFFFFF);
}

// ---------- recordings on disk ----------
struct RecList {
    char name[AUD_MAX_FILES][32];
    uint32_t size[AUD_MAX_FILES];
    int n;
};

static bool is_wav_name(const char *s) {
    size_t n = strlen(s);
    if (n < 5 || n >= 32) return false;
    const char *t = s + n - 4;
    return t[0] == '.' && (t[1] == 'w' || t[1] == 'W') && (t[2] == 'a' || t[2] == 'A') && (t[3] == 'v' || t[3] == 'V');
}

static void list_recordings(RecList *r) {
    r->n = 0;
    DIR *d = opendir(AUD_REC_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && r->n < AUD_MAX_FILES) {
        if (!is_wav_name(e->d_name)) continue;
        char path[96];
        snprintf(path, sizeof(path), AUD_REC_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        snprintf(r->name[r->n], sizeof(r->name[r->n]), "%s", e->d_name);
        r->size[r->n] = (uint32_t)st.st_size;
        r->n++;
    }
    closedir(d);
    for (int i = 1; i < r->n; i++) {               // sort by name
        char tn[32];
        memcpy(tn, r->name[i], sizeof(tn));
        uint32_t ts = r->size[i];
        int j = i - 1;
        while (j >= 0 && strcmp(r->name[j], tn) > 0) {
            memmove(r->name[j + 1], r->name[j], sizeof(r->name[j]));
            r->size[j + 1] = r->size[j];
            j--;
        }
        memcpy(r->name[j + 1], tn, sizeof(tn));
        r->size[j + 1] = ts;
    }
}

static int next_rec_index(void) {
    int best = 0;
    DIR *d = opendir(AUD_REC_DIR);
    if (!d) return 1;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        int v = 0;
        if (sscanf(e->d_name, "rec_%d.wav", &v) == 1 && v > best) best = v;
    }
    closedir(d);
    return best + 1;
}

// ---------- live screen: spectrum analyzer / voice recorder ----------
struct LiveRes {
    Audio *a = nullptr;
    FILE *f = nullptr;
    bool mic = false;
    ~LiveRes() {
        if (f) fclose(f);
        if (mic) mic_stop();
        if (a) heap_caps_free(a);
    }
};

static const float kGains[5] = { -6.0f, 0.0f, 6.0f, 12.0f, 18.0f };

static void run_live(bool record) {
    uint32_t bg = k85_get_bg(), fg = k85_get_fg();
    LiveRes res;
    char path[64] = {0};
    uint32_t data_bytes = 0, max_bytes = 0;

    if (!M5.Mic.isEnabled()) { show_and_wait("No microphone found\nA+B=back"); return; }

    if (record) {
        size_t total = 0, used = 0;
        if (!k85_fs_info(&total, &used) || total < used || total - used < AUD_MIN_FREE) {
            show_and_wait("Not enough free space\nfor a recording\nA+B=back");
            return;
        }
        mkdir(AUD_REC_DIR, 0755);
        int n = next_rec_index();
        if (n > 999) { show_and_wait("Too many recordings\nDelete some first\nA+B=back"); return; }
        snprintf(path, sizeof(path), AUD_REC_DIR "/rec_%03d.wav", n);
        res.f = fopen(path, "wb");
        if (!res.f) { show_and_wait("Cannot create file\nA+B=back"); return; }
        uint8_t h[44];
        wav_header(h, 0);
        if (fwrite(h, 1, 44, res.f) != 44) { fclose(res.f); res.f = nullptr; unlink(path); show_and_wait("Write error\nA+B=back"); return; }
        size_t room = (total - used) - AUD_STOP_FREE;
        size_t cap = (size_t)AUD_MAX_REC_SEC * AUD_RATE * 2;
        max_bytes = (uint32_t)(room < cap ? room : cap);
    }

    res.a = audio_alloc();
    if (!res.a) {
        if (res.f) { fclose(res.f); res.f = nullptr; if (path[0]) unlink(path); }
        show_and_wait("Out of memory\nA+B=back");
        return;
    }
    Audio *a = res.a;

    if (!mic_start()) {
        if (res.f) { fclose(res.f); res.f = nullptr; if (path[0]) unlink(path); }
        show_and_wait("Microphone not\navailable\nA+B=back");
        return;
    }
    res.mic = true;

    Layout L = make_layout();
    draw_static(L, bg, fg, true);

    const int frame = record ? AUD_BUF_N : AUD_FFT_N;
    int idx = 0, gain_idx = 1;
    bool a_prev = false;
    float vu_peak = 0.0f;
    int vu_hold = 0;
    const char *stop_reason = nullptr;
    int64_t t_start = esp_timer_get_time();
    int64_t last_space_check = t_start;

    if (!mic_submit(a->buf[0], frame)) { show_and_wait("Microphone error\nA+B=back"); return; }

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        bool a_now = M5.BtnA.isPressed();
        if (a_now && !a_prev) gain_idx = (gain_idx + 1) % 5;
        a_prev = a_now;

        if (!mic_submit(a->buf[idx ^ 1], frame)) { stop_reason = "Microphone error"; break; }
        int guard = 0;
        while (M5.Mic.isRecording() > 1 && guard++ < 400) vTaskDelay(1);

        const int16_t *cur = a->buf[idx];
        idx ^= 1;

        float rms_db = frame_rms_db(cur, frame);
        analyze(a, cur, frame, kGains[gain_idx]);

        if (record) {
            size_t w = fwrite(cur, 2, (size_t)frame, res.f);
            if (w != (size_t)frame) { stop_reason = "Write error / disk full"; break; }
            data_bytes += (uint32_t)frame * 2;
            if (data_bytes >= max_bytes) { stop_reason = "Maximum length reached"; break; }
            int64_t now = esp_timer_get_time();
            if (now - last_space_check > 1500000) {
                last_space_check = now;
                size_t total = 0, used = 0;
                if (k85_fs_info(&total, &used) && total > used && total - used < AUD_STOP_FREE) { stop_reason = "Disk full"; break; }
            }
        }

        float vu = clamp01((rms_db - AUD_VU_FLOOR) / -AUD_VU_FLOOR);
        if (vu >= vu_peak) { vu_peak = vu; vu_hold = 20; }
        else if (vu_hold > 0) vu_hold--;
        else { vu_peak -= 0.02f; if (vu_peak < vu) vu_peak = vu; }

        draw_bands(a, L, bg);
        draw_vu(L, vu, vu_peak);

        char line[64];
        if (record) {
            uint32_t secs = data_bytes / (AUD_RATE * 2);
            uint32_t maxs = max_bytes / (AUD_RATE * 2);
            bool dot = ((esp_timer_get_time() - t_start) / 500000) % 2 == 0;
            M5.Display.fillCircle(8, 7, 3, dot ? (uint32_t)0xFF0000 : bg);
            snprintf(line, sizeof(line), "REC %02u:%02u / %02u:%02u  A+B=stop", (unsigned)(secs / 60), (unsigned)(secs % 60),
                     (unsigned)(maxs / 60), (unsigned)(maxs % 60));
            put_padded(16, 3, line, 36, fg, bg);
        } else {
            put_padded(4, 3, "SPECTRUM   A=gain   A+B=exit", 38, fg, bg);
        }
        snprintf(line, sizeof(line), "%4.0f dB  gain %+.0f dB", rms_db, kGains[gain_idx]);
        put_padded(4, L.txt_y, line, 38, fg, bg);
    }

    if (record && res.f) {
        if (data_bytes > 0) {
            uint8_t h[44];
            wav_header(h, data_bytes);
            fseek(res.f, 0, SEEK_SET);
            fwrite(h, 1, 44, res.f);
            fclose(res.f);
            res.f = nullptr;
            uint32_t secs = data_bytes / (AUD_RATE * 2);
            char msg[160];
            snprintf(msg, sizeof(msg), "Saved %s\n%u:%02u, %u KB\n%s\nA+B=back", strrchr(path, '/') + 1,
                     (unsigned)(secs / 60), (unsigned)(secs % 60), (unsigned)(data_bytes / 1024),
                     stop_reason ? stop_reason : "Play it in Recordings");
            res.mic = false;
            mic_stop();
            show_and_wait(msg);
        } else {
            fclose(res.f);
            res.f = nullptr;
            unlink(path);
            res.mic = false;
            mic_stop();
            show_and_wait("Nothing recorded\nA+B=back");
        }
    } else if (stop_reason) {
        res.mic = false;
        mic_stop();
        show_and_wait(stop_reason);
    }
}

// ---------- mic test ----------
static void run_mic_test(void) {
    uint32_t bg = k85_get_bg(), fg = k85_get_fg();
    if (!M5.Mic.isEnabled()) { show_and_wait("No microphone found\nA+B=back"); return; }

    LiveRes res;
    res.a = audio_alloc();
    if (!res.a) { show_and_wait("Out of memory\nA+B=back"); return; }
    if (!mic_start()) { show_and_wait("Microphone not\navailable\nA+B=back"); return; }
    res.mic = true;
    Audio *a = res.a;

    Layout L = make_layout();
    L.vu_y = 44;
    L.vu_h = 28;
    draw_static(L, bg, fg, false);
    put_padded(4, 3, "MIC TEST   A+B=exit", 38, fg, bg);
    put_padded(4, 22, "Speak into the microphone", 38, fg, bg);

    const int frame = 256;
    int idx = 0;
    float vu_peak = 0.0f, peak_db = -90.0f;
    int vu_hold = 0, signal_hold = 0;

    if (!mic_submit(a->buf[0], frame)) { show_and_wait("Microphone error\nA+B=back"); return; }
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        if (!mic_submit(a->buf[idx ^ 1], frame)) break;
        int guard = 0;
        while (M5.Mic.isRecording() > 1 && guard++ < 400) vTaskDelay(1);
        const int16_t *cur = a->buf[idx];
        idx ^= 1;

        float db = frame_rms_db(cur, frame);
        float vu = clamp01((db - AUD_VU_FLOOR) / -AUD_VU_FLOOR);
        if (vu >= vu_peak) { vu_peak = vu; vu_hold = 30; peak_db = db; }
        else if (vu_hold > 0) vu_hold--;
        else { vu_peak -= 0.015f; if (vu_peak < vu) { vu_peak = vu; peak_db = db; } }

        if (db > -45.0f) signal_hold = 8; else if (signal_hold > 0) signal_hold--;

        draw_vu(L, vu, vu_peak);

        char line[48];
        M5.Display.setTextSize(2);
        snprintf(line, sizeof(line), "%d dB", (int)db);
        put_padded(4, 88, line, 8, fg, bg);
        M5.Display.setTextSize(1);
        snprintf(line, sizeof(line), "Peak %d dB", (int)peak_db);
        put_padded(4, 112, line, 20, fg, bg);
        put_padded(L.w - 80, 112, signal_hold > 0 ? "SIGNAL" : "silence", 12,
                   signal_hold > 0 ? (uint32_t)0x00FF00 : fg, bg);
    }
}

// ---------- playback ----------
static void play_file(const char *path, const char *title) {
    uint32_t bg = k85_get_bg(), fg = k85_get_fg();
    if (!M5.Speaker.isEnabled()) { show_and_wait("No speaker found\nA+B=back"); return; }
    if (g_config.sound_muted || g_config.sound_volume <= 0) { show_and_wait("Sound is muted or\nvolume is 0\nA+B=back"); return; }

    FILE *f = fopen(path, "rb");
    if (!f) { show_and_wait("Cannot open file\nA+B=back"); return; }
    uint8_t h[44];
    if (fread(h, 1, 44, f) != 44 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVEfmt ", 8) != 0) {
        fclose(f);
        show_and_wait("Unsupported WAV\nA+B=back");
        return;
    }
    uint32_t channels = rd16(h + 22), rate = rd32(h + 24), bits = rd16(h + 34);
    struct stat st;
    uint32_t file_data = (stat(path, &st) == 0 && st.st_size > 44) ? (uint32_t)st.st_size - 44 : 0;
    uint32_t data_size = rd32(h + 40);
    if (data_size == 0 || data_size > file_data) data_size = file_data;
    if (bits != 16 || channels < 1 || channels > 2 || rate < 4000 || rate > 48000 || memcmp(h + 36, "data", 4) != 0 || data_size < 2) {
        fclose(f);
        show_and_wait("Unsupported WAV\n(need 16-bit PCM)\nA+B=back");
        return;
    }

    const size_t kChunk = 4096;
    int16_t *pb[2];
    pb[0] = (int16_t *)psram_alloc(kChunk * 2);
    pb[1] = (int16_t *)psram_alloc(kChunk * 2);
    if (!pb[0] || !pb[1]) {
        free(pb[0]);
        free(pb[1]);
        fclose(f);
        show_and_wait("Out of memory\nA+B=back");
        return;
    }

    k85_apply_sound_volume();
    Layout L = make_layout();
    L.vu_y = 70;
    L.vu_h = 14;
    draw_static(L, bg, fg, false);
    put_padded(4, 3, title, 38, fg, bg);
    put_padded(4, 118, "A+B=stop", 38, fg, bg);
    const int bar_x = 4, bar_y = 28, bar_w = L.w - 8;
    M5.Display.drawRect(bar_x - 1, bar_y - 1, bar_w + 2, 10, fg);

    uint32_t total_vals = data_size / 2;
    uint32_t pos = 0;
    int idx = 0;
    float vu = 0, vu_peak = 0;
    int vu_hold = 0;
    int64_t last_draw = 0;
    bool stopped = false;

    while (!stopped) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); stopped = true; break; }

        if (pos < total_vals && M5.Speaker.isPlaying(0) <= 1) {
            size_t want = total_vals - pos < kChunk ? total_vals - pos : kChunk;
            size_t got = fread(pb[idx], 2, want, f);
            if (got == 0) { pos = total_vals; }
            else {
                double sum = 0;
                for (size_t i = 0; i < got; i++) sum += (double)pb[idx][i] * (double)pb[idx][i];
                float db = to_db((float)sqrt(sum / (double)got));
                vu = clamp01((db - AUD_VU_FLOOR) / -AUD_VU_FLOOR);
                M5.Speaker.playRaw(pb[idx], got, rate, channels == 2, 1, 0, false);
                idx ^= 1;
                pos += (uint32_t)got;
            }
        }
        if (pos >= total_vals && !M5.Speaker.isPlaying(0)) break;

        int64_t now = esp_timer_get_time();
        if (now - last_draw > 60000) {
            last_draw = now;
            if (vu >= vu_peak) { vu_peak = vu; vu_hold = 8; }
            else if (vu_hold > 0) vu_hold--;
            else { vu_peak -= 0.03f; if (vu_peak < vu) vu_peak = vu; }
            draw_vu(L, vu, vu_peak);

            int fill = (int)((uint64_t)bar_w * pos / (total_vals ? total_vals : 1));
            M5.Display.fillRect(bar_x, bar_y, bar_w, 8, bg);
            M5.Display.fillRect(bar_x, bar_y, fill, 8, (uint32_t)0x00A0FF);
            uint32_t cur_s = pos / channels / rate, tot_s = total_vals / channels / rate;
            char line[48];
            snprintf(line, sizeof(line), "%u:%02u / %u:%02u", (unsigned)(cur_s / 60), (unsigned)(cur_s % 60),
                     (unsigned)(tot_s / 60), (unsigned)(tot_s % 60));
            put_padded(4, 42, line, 20, fg, bg);
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }

    M5.Speaker.stop();
    fclose(f);
    free(pb[0]);
    free(pb[1]);
}

static void recordings_menu(void) {
    mkdir(AUD_REC_DIR, 0755);
    while (true) {
        RecList r;
        list_recordings(&r);
        if (r.n == 0) { show_and_wait("No recordings yet\nUse Record voice\nA+B=back"); return; }

        char labels[AUD_MAX_FILES][48];
        const char *items[AUD_MAX_FILES + 1];
        for (int i = 0; i < r.n; i++) {
            unsigned secs = r.size[i] > 44 ? (r.size[i] - 44) / (AUD_RATE * 2) : 0;
            snprintf(labels[i], sizeof(labels[i]), "%.12s %u:%02u %uK", r.name[i], secs / 60, secs % 60, (unsigned)(r.size[i] / 1024));
            items[i] = labels[i];
        }
        items[r.n] = "Back";
        int idx = k85_run_list_menu("RECORDINGS", items, r.n + 1, nullptr);
        if (idx < 0 || idx >= r.n) return;

        char path[96];
        snprintf(path, sizeof(path), AUD_REC_DIR "/%s", r.name[idx]);
        static const char *const actions[3] = { "Play", "Delete", "Back" };
        int act = k85_run_list_menu(r.name[idx], actions, 3, nullptr);
        if (act == 0) {
            play_file(path, r.name[idx]);
        } else if (act == 1) {
            if (confirm_msg("Delete this recording?\nB=confirm A+B=cancel")) unlink(path);
        }
    }
}

// ---------- speaker test ----------
static void speaker_menu(void) {
    while (true) {
        char vol[40];
        snprintf(vol, sizeof(vol), "Volume: %d%%%s", g_config.sound_volume, g_config.sound_muted ? " (muted)" : "");
        const char *items[] = { "Tone 440 Hz", "Tone 1 kHz", "Tone 4 kHz", "Sweep 100 Hz - 8 kHz", vol, "Back" };
        int idx = k85_run_list_menu("SPEAKER TEST", items, 6, nullptr);
        if (idx < 0 || idx == 5) return;

        if (idx == 4) {
            int v = g_config.sound_volume;
            int next = v < 25 ? 25 : (v < 50 ? 50 : (v < 75 ? 75 : (v < 100 ? 100 : 0)));
            g_config.sound_volume = next;
            k85_config_save();
            k85_apply_sound_volume();
            continue;
        }
        if (!M5.Speaker.isEnabled()) { show_and_wait("No speaker found\nA+B=back"); continue; }
        if (g_config.sound_muted || g_config.sound_volume <= 0) { show_and_wait("Sound is muted or\nvolume is 0\nA+B=back"); continue; }

        k85_apply_sound_volume();
        if (idx <= 2) {
            static const int freqs[3] = { 440, 1000, 4000 };
            char msg[48];
            snprintf(msg, sizeof(msg), "Playing %d Hz...", freqs[idx]);
            k85_show_message(msg);
            M5.Speaker.tone((float)freqs[idx], 1500);
            vTaskDelay(pdMS_TO_TICKS(1600));
        } else {
            k85_show_message("Sweep 100 Hz - 8 kHz...");
            for (int i = 0; i < 40; i++) {
                float fq = 100.0f * powf(80.0f, (float)i / 39.0f);
                M5.Speaker.tone(fq, 90);
                vTaskDelay(pdMS_TO_TICKS(75));
            }
        }
        k85_speaker_stop();
    }
}

// ---------- entry ----------
void k85_run_audio_tool(void) {
    static const char *items[] = { "Spectrum", "Mic test", "Record voice", "Recordings", "Speaker test", "Back" };
    while (true) {
        int idx = k85_run_list_menu("AUDIO", items, 6, nullptr);
        if (idx < 0 || idx == 5) return;
        if (idx == 0) run_live(false);
        else if (idx == 1) run_mic_test();
        else if (idx == 2) run_live(true);
        else if (idx == 3) recordings_menu();
        else if (idx == 4) speaker_menu();
    }
}

#pragma GCC diagnostic pop
