#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "sysmon_tool.h"
#include "common.h"
#include "input.h"
#include "config.h"
#include "list_menu.h"
#include "battery.h"
#include "wifi.h"
#include "theme.h"
#include "device.h"

#include "M5Unified.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

static char *xalloc(size_t n) {
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    if (!p) p = calloc(1, n);
    return (char *)p;
}

// 0 = green, 0.5 = yellow, 1 = red
static uint32_t heat(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float r = 2.0f * t, g = 2.0f * (1.0f - t);
    int ri = (int)(255.0f * (r > 1.0f ? 1.0f : r));
    int gi = (int)(255.0f * (g > 1.0f ? 1.0f : g));
    return ((uint32_t)ri << 16) | ((uint32_t)gi << 8);
}

// ======================= Performance =======================
#define PERF_MAX_PTS 160
#define PERF_MAX_TASKS 40

struct PerfData {
    uint8_t cpu[PERF_MAX_PTS];
    uint8_t ram[PERF_MAX_PTS];
    uint8_t psram[PERF_MAX_PTS];
    int n;
};

struct CpuSampler {
    bool primed = false;
    uint32_t prev_total = 0, prev_idle = 0;
};

// Share of non-idle CPU time since the previous call (0..100).
// Needs FreeRTOS run-time stats; returns false if they are disabled or on the first call.
static bool cpu_sample(CpuSampler *s, TaskStatus_t *tasks, int *load) {
    UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count > PERF_MAX_TASKS) count = PERF_MAX_TASKS;
    uint32_t total = 0;
    count = uxTaskGetSystemState(tasks, count, &total);
    if (total == 0) return false;                        // run-time stats are off
    uint32_t idle = 0;
    for (UBaseType_t i = 0; i < count; i++) {
        if (strncmp(tasks[i].pcTaskName, "IDLE", 4) == 0) idle += tasks[i].ulRunTimeCounter;
    }
    bool ok = false;
    if (s->primed) {
        uint32_t dt = total - s->prev_total, di = idle - s->prev_idle;
        if (dt > 0) {
            float busy = 100.0f - 100.0f * (float)di / ((float)dt * (float)portNUM_PROCESSORS);
            if (busy < 0.0f) busy = 0.0f;
            if (busy > 100.0f) busy = 100.0f;
            *load = (int)(busy + 0.5f);
            ok = true;
        }
    }
    s->primed = true;
    s->prev_total = total;
    s->prev_idle = idle;
    return ok;
}

static void perf_push(uint8_t *arr, int n, int pts, uint8_t v) {
    if (n < pts) { arr[n] = v; return; }
    memmove(arr, arr + 1, (size_t)(pts - 1));
    arr[pts - 1] = v;
}

static void draw_graph(int x, int y, int w, int h, const uint8_t *v, int n, int pts, uint32_t fg, uint32_t bg) {
    auto &d = M5.Display;
    d.fillRect(x, y, w, h, bg);
    d.drawRect(x, y, w, h, fg);
    for (int i = 0; i < n; i++) {
        int hh = (int)v[i] * (h - 2) / 100;
        if (hh <= 0) continue;
        int xi = x + 1 + (pts - n + i) * 2;
        d.fillRect(xi, y + h - 1 - hh, 2, hh, heat((float)v[i] / 100.0f));
    }
}

void k85_run_performance(void) {
    uint32_t bg = k85_get_bg(), fg = k85_get_fg();
    auto &d = M5.Display;

    PerfData *pd = (PerfData *)xalloc(sizeof(PerfData));
    TaskStatus_t *tasks = (TaskStatus_t *)xalloc(sizeof(TaskStatus_t) * PERF_MAX_TASKS);
    if (!pd || !tasks) { free(pd); free(tasks); show_and_wait("Out of memory\nA+B=back"); return; }

    const int W = d.width(), H = d.height();
    int pts = (W - 10) / 2;
    if (pts > PERF_MAX_PTS) pts = PERF_MAX_PTS;
    const int gx = 4, gw = pts * 2 + 2;
    const int gh = (H - 16 - 6) / 3;
    const int gy[3] = { 14, 14 + gh + 3, 14 + 2 * (gh + 3) };

    d.fillScreen(bg);
    d.setTextSize(1);

    CpuSampler cs;
    int interval_ms = 1000;
    bool paused = false, a_prev = false, cpu_ok_ever = false;
    int64_t last = 0;
    int cpu_now = 0;

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        bool a_now = M5.BtnA.isPressed();
        if (a_now && !a_prev) paused = !paused;
        a_prev = a_now;
        if (k85_btn_b_pressed()) interval_ms = interval_ms == 1000 ? 2000 : (interval_ms == 2000 ? 500 : 1000);

        int64_t now = esp_timer_get_time();
        if (!paused && (last == 0 || now - last >= (int64_t)interval_ms * 1000)) {
            last = now;
            bool cpu_ok = cpu_sample(&cs, tasks, &cpu_now);
            if (cpu_ok) cpu_ok_ever = true;

            size_t it = heap_caps_get_total_size(MALLOC_CAP_INTERNAL), ifr = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM), pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            int ram_pct = it ? (int)((it - ifr) * 100 / it) : 0;
            int ps_pct = pt ? (int)((pt - pf) * 100 / pt) : 0;

            perf_push(pd->cpu, pd->n, pts, (uint8_t)(cpu_ok ? cpu_now : 0));
            perf_push(pd->ram, pd->n, pts, (uint8_t)ram_pct);
            perf_push(pd->psram, pd->n, pts, (uint8_t)ps_pct);
            if (pd->n < pts) pd->n++;

            draw_graph(gx, gy[0], gw, gh, pd->cpu, pd->n, pts, fg, bg);
            draw_graph(gx, gy[1], gw, gh, pd->ram, pd->n, pts, fg, bg);
            draw_graph(gx, gy[2], gw, gh, pd->psram, pd->n, pts, fg, bg);

            char line[64];
            d.setTextColor((uint32_t)fg);
            if (cpu_ok_ever) snprintf(line, sizeof(line), "CPU %d%%", cpu_now);
            else snprintf(line, sizeof(line), "CPU n/a (run-time stats off)");
            d.setCursor(gx + 4, gy[0] + 3); d.print(line);
            snprintf(line, sizeof(line), "RAM %uK/%uK  %d%%  min free %uK", (unsigned)((it - ifr) / 1024), (unsigned)(it / 1024), ram_pct,
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
            d.setCursor(gx + 4, gy[1] + 3); d.print(line);
            if (pt) snprintf(line, sizeof(line), "PSRAM %uK/%uK  %d%%", (unsigned)((pt - pf) / 1024), (unsigned)(pt / 1024), ps_pct);
            else snprintf(line, sizeof(line), "PSRAM: none");
            d.setCursor(gx + 4, gy[2] + 3); d.print(line);
        }

        char hdr[48];
        snprintf(hdr, sizeof(hdr), "PERF %.1fs %s A=pause B=speed", (double)interval_ms / 1000.0, paused ? "[PAUSED]" : "        ");
        d.setTextColor(fg, bg);
        d.setCursor(4, 3);
        d.print(hdr);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    free(pd);
    free(tasks);
}

// ======================= Device manager =======================
#define DEV_ROWS 14

struct DevRows { char s[DEV_ROWS][40]; int n; };

static void dev_add(DevRows *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void dev_add(DevRows *r, const char *fmt, ...) {
    if (r->n >= DEV_ROWS) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->s[r->n], sizeof(r->s[r->n]), fmt, ap);
    va_end(ap);
    r->n++;
}

static void build_rows(DevRows *r) {
    r->n = 0;
    dev_add(r, "Device:    %.26s", k85_get_device_name());
    dev_add(r, "Display:   %dx%d", (int)M5.Display.width(), (int)M5.Display.height());
    dev_add(r, "IMU:       %s", M5.Imu.isEnabled() ? "found" : "not found");
    dev_add(r, "Microphone:%s", M5.Mic.isEnabled() ? " found" : " not found");
    if (M5.Speaker.isEnabled()) dev_add(r, "Speaker:   found, vol %d%%%s", g_config.sound_volume, g_config.sound_muted ? " muted" : "");
    else dev_add(r, "Speaker:   not found");
    if (M5.Rtc.isEnabled()) {
        auto dt = M5.Rtc.getDateTime();
        dev_add(r, "RTC:       %02d:%02d:%02d", (int)dt.time.hours, (int)dt.time.minutes, (int)dt.time.seconds);
    } else {
        dev_add(r, "RTC:       not found");
    }

    int pct = k85_get_battery();
    int32_t mv = M5.Power.getBatteryVoltage();
    if (pct >= 0) dev_add(r, "Battery:   %d%% %.2fV%s", pct, mv / 1000.0f, k85_is_charging() ? " chg" : "");
    else dev_add(r, "Battery:   n/a");

    if (g_config.wifi_disabled) dev_add(r, "WiFi:      disabled (BIOS)");
    else if (k85_wifi_is_connected()) dev_add(r, "WiFi:      %.14s %ddBm", g_config.wifi_ssid, k85_wifi_get_rssi());
    else dev_add(r, "WiFi:      not connected");
    dev_add(r, "Bluetooth: %s", g_config.bt_disabled ? "disabled (BIOS)" : "enabled");

    size_t pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM), pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (pt) dev_add(r, "PSRAM:     %uK, free %uK", (unsigned)(pt / 1024), (unsigned)(pf / 1024));
    else dev_add(r, "PSRAM:     none");

    uint32_t flash = 0;
    esp_flash_get_size(nullptr, &flash);
    dev_add(r, "Flash:     %u MB", (unsigned)(flash / (1024 * 1024)));

    size_t ft = 0, fu = 0;
    if (k85_fs_info(&ft, &fu) && ft) dev_add(r, "Storage:   %uK, used %u%%", (unsigned)(ft / 1024), (unsigned)(fu * 100 / ft));
    else dev_add(r, "Storage:   n/a");

    bool found[0x78];
    memset(found, 0, sizeof(found));
    int cnt = 0;
    char addrs[32] = "";
    M5.Ex_I2C.scanID(found, 100000);   // returns void in this M5Unified version
    {
        for (int a = 0; a < 0x78; a++) {
            if (!found[a]) continue;
            cnt++;
            size_t l = strlen(addrs);
            if (l + 4 < sizeof(addrs)) snprintf(addrs + l, sizeof(addrs) - l, "%02X ", a);
        }
    }
    if (cnt) dev_add(r, "I2C Grove: %d: %.20s", cnt, addrs);
    else dev_add(r, "I2C Grove: no devices");
}

void k85_run_device_manager(void) {
    DevRows *r = (DevRows *)xalloc(sizeof(DevRows));
    if (!r) { show_and_wait("Out of memory\nA+B=back"); return; }
    k85_show_message("Scanning devices...");
    build_rows(r);
    while (true) {
        const char *items[DEV_ROWS + 2];
        for (int i = 0; i < r->n; i++) items[i] = r->s[i];
        items[r->n] = "Refresh";
        items[r->n + 1] = "Back";
        int idx = k85_run_list_menu("DEVICE MANAGER", items, r->n + 2, nullptr);
        if (idx < 0 || idx == r->n + 1) break;
        if (idx == r->n) {
            k85_show_message("Scanning devices...");
            build_rows(r);
            continue;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "%s\nA+B=back", r->s[idx]);
        show_and_wait(msg);
    }
    free(r);
}

#pragma GCC diagnostic pop
