#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "cron.h"
#include "script_engine.h"
#include "config.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "k85_cron";

#define CRON_PATH     "/littlefs/crontab"
#define CRON_LOG      "/littlefs/cron.log"
#define CRON_MAX_JOBS 16
#define CRON_LOG_MAX  8192
#define CRON_OUT_SIZE 1536

struct Job {
    bool used;
    bool dom_any;
    bool dow_any;
    uint64_t mins, hours, doms, mons, dows;
    char sched[48];
    char cmd[96];
};

static Job s_jobs[CRON_MAX_JOBS];
static bool s_run = false;
static TaskHandle_t s_task = nullptr;

static SemaphoreHandle_t cron_mtx(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}

struct CronLock {
    CronLock() { xSemaphoreTake(cron_mtx(), portMAX_DELAY); }
    ~CronLock() { xSemaphoreGive(cron_mtx()); }
};

static char *cron_alloc(size_t n) {
    char *p = (char *)heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    if (!p) p = (char *)calloc(1, n);
    return p;
}

// ---------- parsing ----------
static bool all_digits(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 3) return false;
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static bool parse_field(const char *field, int lo, int hi, uint64_t *mask) {
    *mask = 0;
    char buf[40];
    if (strlen(field) >= sizeof(buf)) return false;
    snprintf(buf, sizeof(buf), "%s", field);

    char *save = nullptr;
    for (char *item = strtok_r(buf, ",", &save); item; item = strtok_r(nullptr, ",", &save)) {
        int step = 1;
        bool has_step = false;
        char *slash = strchr(item, '/');
        if (slash) {
            *slash = 0;
            if (!all_digits(slash + 1)) return false;
            step = atoi(slash + 1);
            if (step <= 0) return false;
            has_step = true;
        }
        int a = lo, b = hi;
        if (strcmp(item, "*") != 0) {
            char *dash = strchr(item, '-');
            if (dash) {
                *dash = 0;
                if (!all_digits(item) || !all_digits(dash + 1)) return false;
                a = atoi(item);
                b = atoi(dash + 1);
            } else {
                if (!all_digits(item)) return false;
                a = atoi(item);
                b = has_step ? hi : a;
            }
        }
        if (a < lo || b > hi || a > b) return false;
        for (int v = a; v <= b; v += step) *mask |= (1ULL << v);
    }
    return *mask != 0;
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool parse_line(const char *line, Job *j) {
    char fields[5][40];
    const char *p = line;
    for (int i = 0; i < 5; i++) {
        p = skip_ws(p);
        size_t n = 0;
        while (p[n] && p[n] != ' ' && p[n] != '\t') n++;
        if (n == 0 || n >= sizeof(fields[i])) return false;
        memcpy(fields[i], p, n);
        fields[i][n] = 0;
        p += n;
    }
    p = skip_ws(p);
    if (!*p) return false;

    memset(j, 0, sizeof(*j));
    if (!parse_field(fields[0], 0, 59, &j->mins)) return false;
    if (!parse_field(fields[1], 0, 23, &j->hours)) return false;
    if (!parse_field(fields[2], 1, 31, &j->doms)) return false;
    if (!parse_field(fields[3], 1, 12, &j->mons)) return false;
    if (!parse_field(fields[4], 0, 7, &j->dows)) return false;
    if (j->dows & (1ULL << 7)) j->dows |= 1ULL;   // 7 = Sunday = 0

    j->dom_any = fields[2][0] == '*';
    j->dow_any = fields[4][0] == '*';
    snprintf(j->cmd, sizeof(j->cmd), "%s", p);
    snprintf(j->sched, sizeof(j->sched), "%s %s %s %s %s", fields[0], fields[1], fields[2], fields[3], fields[4]);
    j->used = true;
    return true;
}

int k85_cron_reload(void) {
    Job *nj = (Job *)cron_alloc(sizeof(Job) * CRON_MAX_JOBS);
    char *buf = cron_alloc(2049);
    if (!nj || !buf) { free(nj); free(buf); return -1; }

    int count = 0;
    FILE *f = fopen(CRON_PATH, "r");
    if (f) {
        size_t r = fread(buf, 1, 2048, f);
        fclose(f);
        buf[r] = 0;

        char *pos = buf;
        while (pos && *pos && count < CRON_MAX_JOBS) {
            char *nl = strchr(pos, '\n');
            if (nl) *nl = 0;
            char *line = pos;
            pos = nl ? nl + 1 : nullptr;

            size_t n = strlen(line);
            while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
            const char *t = skip_ws(line);
            if (!*t || *t == '#') continue;

            if (parse_line(t, &nj[count])) count++;
            else ESP_LOGW(TAG, "bad crontab line ignored: %s", t);
        }
    }

    {
        CronLock lk;
        memcpy(s_jobs, nj, sizeof(Job) * CRON_MAX_JOBS);
    }
    free(nj);
    free(buf);
    ESP_LOGI(TAG, "%d job(s) loaded", count);
    return count;
}

int k85_cron_job_count(void) {
    CronLock lk;
    int n = 0;
    for (int i = 0; i < CRON_MAX_JOBS; i++) if (s_jobs[i].used) n++;
    return n;
}

int k85_cron_list(char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = 0;
    size_t used = 0;
    int n = 0;
    {
        CronLock lk;
        for (int i = 0; i < CRON_MAX_JOBS; i++) {
            if (!s_jobs[i].used) continue;
            n++;
            int w = snprintf(out + used, out_size - used, "%d) %s  %s\r\n", n, s_jobs[i].sched, s_jobs[i].cmd);
            if (w > 0 && (size_t)w < out_size - used) used += (size_t)w; else break;
        }
    }
    if (n == 0) snprintf(out, out_size, "(no jobs)\r\n");
    return n;
}

// ---------- running ----------
static bool job_matches(const Job *j, const struct tm *tm) {
    if (!((j->mins >> tm->tm_min) & 1ULL)) return false;
    if (!((j->hours >> tm->tm_hour) & 1ULL)) return false;
    if (!((j->mons >> (tm->tm_mon + 1)) & 1ULL)) return false;
    bool dom = ((j->doms >> tm->tm_mday) & 1ULL) != 0;
    bool dow = ((j->dows >> tm->tm_wday) & 1ULL) != 0;
    if (!j->dom_any && !j->dow_any) return dom || dow;   // classic cron: OR when both restricted
    return dom && dow;
}

static void log_run(const struct tm *tm, const char *cmd, const char *out) {
    ESP_LOGI(TAG, "run: %s", cmd);
    struct stat st;
    if (stat(CRON_LOG, &st) == 0 && st.st_size > CRON_LOG_MAX) unlink(CRON_LOG);
    FILE *f = fopen(CRON_LOG, "a");
    if (!f) return;
    fprintf(f, "%02d:%02d %s\n", tm->tm_hour, tm->tm_min, cmd);
    char first[121];
    size_t n = 0;
    while (out[n] && out[n] != '\r' && out[n] != '\n' && n < 120) { first[n] = out[n]; n++; }
    first[n] = 0;
    if (n) fprintf(f, "  > %s\n", first);
    fclose(f);
}

static void run_due(time_t now, char *outbuf) {
    time_t local = now + (time_t)g_config.utc_offset * 3600;
    struct tm tm;
    gmtime_r(&local, &tm);

    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        char cmd[96];
        bool hit = false;
        {
            CronLock lk;
            if (s_jobs[i].used && job_matches(&s_jobs[i], &tm)) {
                snprintf(cmd, sizeof(cmd), "%s", s_jobs[i].cmd);
                hit = true;
            }
        }
        if (!hit) continue;
        outbuf[0] = 0;
        k85_script_exec(cmd, outbuf, CRON_OUT_SIZE);
        log_run(&tm, cmd, outbuf);
    }
}

static void cron_task(void *arg) {
    (void)arg;
    char *outbuf = cron_alloc(CRON_OUT_SIZE);
    time_t last_minute = 0;

    while (true) {
        time_t now = time(nullptr);
        if (outbuf && now > 1700000000) {   // clock is set (NTP)
            time_t minute = now / 60;
            if (minute != last_minute) {
                last_minute = minute;
                run_due(now, outbuf);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));

        xSemaphoreTake(cron_mtx(), portMAX_DELAY);
        if (!s_run) {
            s_task = nullptr;
            xSemaphoreGive(cron_mtx());
            break;
        }
        xSemaphoreGive(cron_mtx());
    }
    free(outbuf);
    vTaskDelete(nullptr);
}

bool k85_cron_start(void) {
    k85_cron_reload();
    CronLock lk;
    s_run = true;
    if (!s_task) {
        if (xTaskCreate(cron_task, "k85_cron", 6144, nullptr, 3, &s_task) != pdPASS) {
            s_task = nullptr;
            s_run = false;
        }
    }
    return s_run;
}

void k85_cron_stop(void) {
    CronLock lk;
    s_run = false;
}

bool k85_cron_is_running(void) {
    CronLock lk;
    return s_run && s_task != nullptr;
}

#pragma GCC diagnostic pop
