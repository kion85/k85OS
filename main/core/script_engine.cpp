#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "script_engine.h"
#include "lock_auth.h"
#include "shell_ext.h"
#include "screenshot.h"
#include "cron.h"
#include "svc_cfg.h"
#include "fs_guard.h"
#include "shell_commands.h"
#include "config.h"
#include "../net/ftp_server.h"
#include "../net/ssh_server.h"
#include "../net/wifi.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

static const char *TAG = "k85_script";

#define K85_STARTUP_PATH "/littlefs/startup.sh"
#define K85_MAX_VARS     8
#define K85_MAX_ALIASES  8
#define K85_MAX_NEST     4

struct Var   { char name[16]; char val[64]; };
struct Alias { char name[16]; char cmd[96]; };

static Var   s_vars[K85_MAX_VARS];
static Alias s_aliases[K85_MAX_ALIASES];
static int   s_nest = 0;

static SemaphoreHandle_t tbl_mtx(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}

struct TblLock {
    TblLock() { xSemaphoreTake(tbl_mtx(), portMAX_DELAY); }
    ~TblLock() { xSemaphoreGive(tbl_mtx()); }
};

// Commands that already exist: aliases with these names would never fire.
static const char *const kBuiltins[] = {
    "help", "ps", "htop", "info", "free", "uptime", "battery", "wifi", "imu", "ls", "cat",
    "wifi-on", "wifi-off", "brightness", "volume", "panic-test", "reboot", "exit",
    "echo", "set", "alias", "unalias", "sleep", "sh", "cron", "ftp", "ftppass",
    "grep", "head", "tail", "wc", "cp", "mv", "rm", "mkdir", "touch", "date", "df", "du",
    "hostname", "ifconfig", "nslookup", "wget", "curl", "watch", "neofetch", "at", "tar", "scrprobe", "scrshot",
};

static bool is_builtin(const char *name) {
    for (size_t i = 0; i < sizeof(kBuiltins) / sizeof(kBuiltins[0]); i++) {
        if (!strcmp(kBuiltins[i], name)) return true;
    }
    return false;
}

static char *alloc_buf(size_t n) {
    char *p = (char *)heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    if (!p) p = (char *)calloc(1, n);
    return p;
}

static bool valid_name(const char *s, size_t maxlen, bool allow_dash) {
    size_t n = strlen(s);
    if (n == 0 || n >= maxlen) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || (allow_dash && c == '-'))) return false;
    }
    return true;
}

static char *trim_inplace(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

// Copies the first space-separated token into tok, *rest points after it.
static void first_token(const char *s, char *tok, size_t n, const char **rest) {
    while (*s == ' ') s++;
    size_t k = 0;
    while (s[k] && s[k] != ' ' && k + 1 < n) { tok[k] = s[k]; k++; }
    tok[k] = 0;
    const char *r = s + k;
    while (*r && *r != ' ') r++;
    while (*r == ' ') r++;
    *rest = r;
}

static void out_append(char *out, size_t out_size, size_t *used, const char *s) {
    if (!out || out_size == 0) return;
    size_t n = strlen(s);
    if (*used + n + 1 > out_size) n = (out_size > *used + 1) ? out_size - *used - 1 : 0;
    memcpy(out + *used, s, n);
    *used += n;
    out[*used] = 0;
}

// ---------- variables ----------
static void lookup_var(const char *name, char *val, size_t n) {
    val[0] = 0;
    if (!strcmp(name, "IP")) { snprintf(val, n, "%s", k85_wifi_get_ip_str()); return; }
    if (!strcmp(name, "SSID")) { snprintf(val, n, "%s", g_config.wifi_ssid); return; }
    TblLock lk;
    for (int i = 0; i < K85_MAX_VARS; i++) {
        if (s_vars[i].name[0] && !strcmp(s_vars[i].name, name)) {
            snprintf(val, n, "%s", s_vars[i].val);
            return;
        }
    }
}

// $NAME -> value. Built-ins: $IP, $SSID. Unknown variables expand to nothing.
static void expand_vars(const char *in, char *out, size_t n) {
    size_t o = 0;
    size_t i = 0;
    while (in[i] && o + 1 < n) {
        if (in[i] == '$' && (isalpha((unsigned char)in[i + 1]) || in[i + 1] == '_')) {
            char name[16];
            size_t k = 0;
            i++;
            while (in[i] && (isalnum((unsigned char)in[i]) || in[i] == '_')) {
                if (k < sizeof(name) - 1) name[k++] = in[i];
                i++;
            }
            name[k] = 0;
            char val[64];
            lookup_var(name, val, sizeof(val));
            for (size_t v = 0; val[v] && o + 1 < n; v++) out[o++] = val[v];
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = 0;
}

// ---------- script execution ----------
static void exec_line(const char *raw, char *out, size_t out_size, bool *stop) {
    out[0] = 0;
    char expanded[192];
    expand_vars(raw, expanded, sizeof(expanded));
    char *line = trim_inplace(expanded);
    if (!*line || *line == '#') return;

    char first[32];
    const char *rest;
    first_token(line, first, sizeof(first), &rest);

    if (!strcmp(first, "exit")) {
        if (stop) *stop = true;
        return;
    }
    if (!strcmp(first, "reboot")) {
        ESP_LOGW(TAG, "script requested reboot");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }
    k85_shell_run_command(line, out, out_size);
}

void k85_script_exec(const char *line, char *out, size_t out_size) {
    bool stop = false;
    exec_line(line, out, out_size, &stop);
}

static bool nest_enter(void) {
    TblLock lk;
    if (s_nest >= K85_MAX_NEST) return false;
    s_nest++;
    return true;
}

static void nest_leave(void) {
    TblLock lk;
    if (s_nest > 0) s_nest--;
}

int k85_script_run_file(const char *path, char *out, size_t out_size) {
    if (out && out_size) out[0] = 0;

    const char *p = path ? path : "";
    while (*p == ' ') p++;
    if (!*p) {
        if (out) snprintf(out, out_size, "sh: usage: sh <file>\r\n");
        return -1;
    }

    char full[192];
    if (!strncmp(p, "/littlefs/", 10)) snprintf(full, sizeof(full), "%s", p);
    else snprintf(full, sizeof(full), "/littlefs/%s", *p == '/' ? p + 1 : p);
    // trailing spaces from the command line
    size_t fl = strlen(full);
    while (fl && (full[fl - 1] == ' ' || full[fl - 1] == '\r' || full[fl - 1] == '\n')) full[--fl] = 0;

    if (strstr(full, "..") || k85_is_blocked_path(full)) {
        if (out) snprintf(out, out_size, "sh: access denied\r\n");
        return -1;
    }
    if (!nest_enter()) {
        if (out) snprintf(out, out_size, "sh: nested too deep\r\n");
        return -1;
    }

    FILE *f = fopen(full, "r");
    if (!f) {
        nest_leave();
        if (out) snprintf(out, out_size, "sh: cannot open %s\r\n", full);
        return -1;
    }

    const size_t kTmp = 1536;
    char *tmp = alloc_buf(kTmp);
    if (!tmp) {
        fclose(f);
        nest_leave();
        if (out) snprintf(out, out_size, "sh: out of memory\r\n");
        return -1;
    }

    char line[160];
    size_t used = 0;
    int lines = 0;
    bool stop = false;
    while (!stop && fgets(line, sizeof(line), f)) {
        // over-long line: drop the rest of it
        if (!strchr(line, '\n') && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
        }
        lines++;
        exec_line(line, tmp, kTmp, &stop);
        if (tmp[0]) {
            ESP_LOGI(TAG, "%s", tmp);
            out_append(out, out_size, &used, tmp);
        }
    }
    fclose(f);
    free(tmp);
    nest_leave();
    return lines;
}

// ---------- extra shell commands ----------
static void ext_set(const char *arg, char *out, size_t out_size) {
    out[0] = 0;
    while (*arg == ' ') arg++;

    if (!*arg) {
        size_t used = 0;
        TblLock lk;
        for (int i = 0; i < K85_MAX_VARS; i++) {
            if (!s_vars[i].name[0]) continue;
            int w = snprintf(out + used, out_size - used, "%s=%s\r\n", s_vars[i].name, s_vars[i].val);
            if (w > 0 && (size_t)w < out_size - used) used += (size_t)w; else break;
        }
        return;
    }

    const char *eq = strchr(arg, '=');
    if (!eq) { snprintf(out, out_size, "usage: set NAME=value\r\n"); return; }

    char name[16];
    size_t nl = (size_t)(eq - arg);
    if (nl == 0 || nl >= sizeof(name)) { snprintf(out, out_size, "set: bad name\r\n"); return; }
    memcpy(name, arg, nl);
    name[nl] = 0;
    if (!valid_name(name, sizeof(name), false)) { snprintf(out, out_size, "set: bad name\r\n"); return; }

    char ex[128];
    expand_vars(eq + 1, ex, sizeof(ex));
    const char *v = ex;
    while (*v == ' ') v++;

    TblLock lk;
    int slot = -1;
    for (int i = 0; i < K85_MAX_VARS; i++) {
        if (!strcmp(s_vars[i].name, name)) { slot = i; break; }
        if (slot < 0 && !s_vars[i].name[0]) slot = i;
    }
    if (slot < 0) { snprintf(out, out_size, "set: too many variables\r\n"); return; }
    snprintf(s_vars[slot].name, sizeof(s_vars[slot].name), "%s", name);
    snprintf(s_vars[slot].val, sizeof(s_vars[slot].val), "%s", v);
}

static void ext_alias(const char *arg, char *out, size_t out_size) {
    out[0] = 0;
    while (*arg == ' ') arg++;

    if (!*arg) {
        size_t used = 0;
        TblLock lk;
        for (int i = 0; i < K85_MAX_ALIASES; i++) {
            if (!s_aliases[i].name[0]) continue;
            int w = snprintf(out + used, out_size - used, "alias %s='%s'\r\n", s_aliases[i].name, s_aliases[i].cmd);
            if (w > 0 && (size_t)w < out_size - used) used += (size_t)w; else break;
        }
        return;
    }

    const char *eq = strchr(arg, '=');
    if (!eq) { snprintf(out, out_size, "usage: alias name=command\r\n"); return; }

    char name[16];
    size_t nl = (size_t)(eq - arg);
    if (nl == 0 || nl >= sizeof(name)) { snprintf(out, out_size, "alias: bad name\r\n"); return; }
    memcpy(name, arg, nl);
    name[nl] = 0;
    if (!valid_name(name, sizeof(name), true)) { snprintf(out, out_size, "alias: bad name\r\n"); return; }
    if (is_builtin(name)) { snprintf(out, out_size, "alias: '%s' is a built-in command\r\n", name); return; }

    char ex[128];
    expand_vars(eq + 1, ex, sizeof(ex));
    char *cmd = trim_inplace(ex);
    if (!*cmd || strlen(cmd) >= sizeof(s_aliases[0].cmd)) { snprintf(out, out_size, "alias: bad command\r\n"); return; }

    TblLock lk;
    int slot = -1;
    for (int i = 0; i < K85_MAX_ALIASES; i++) {
        if (!strcmp(s_aliases[i].name, name)) { slot = i; break; }
        if (slot < 0 && !s_aliases[i].name[0]) slot = i;
    }
    if (slot < 0) { snprintf(out, out_size, "alias: too many aliases\r\n"); return; }
    snprintf(s_aliases[slot].name, sizeof(s_aliases[slot].name), "%s", name);
    snprintf(s_aliases[slot].cmd, sizeof(s_aliases[slot].cmd), "%s", cmd);
}

static void ext_unalias(const char *arg, char *out, size_t out_size) {
    out[0] = 0;
    char name[16];
    const char *rest;
    first_token(arg, name, sizeof(name), &rest);
    TblLock lk;
    for (int i = 0; i < K85_MAX_ALIASES; i++) {
        if (s_aliases[i].name[0] && !strcmp(s_aliases[i].name, name)) {
            memset(&s_aliases[i], 0, sizeof(s_aliases[i]));
            return;
        }
    }
    snprintf(out, out_size, "unalias: %s: not found\r\n", name);
}

static bool alias_exists(const char *name) {
    TblLock lk;
    for (int i = 0; i < K85_MAX_ALIASES; i++) {
        if (s_aliases[i].name[0] && !strcmp(s_aliases[i].name, name)) return true;
    }
    return false;
}

static bool try_alias(const char *cmd, const char *arg, char *out, size_t out_size) {
    char acmd[96] = {0};
    bool found = false;
    {
        TblLock lk;
        for (int i = 0; i < K85_MAX_ALIASES; i++) {
            if (s_aliases[i].name[0] && !strcmp(s_aliases[i].name, cmd)) {
                snprintf(acmd, sizeof(acmd), "%s", s_aliases[i].cmd);
                found = true;
                break;
            }
        }
    }
    if (!found) return false;

    char full[224];
    snprintf(full, sizeof(full), "%s%s%s", acmd, arg[0] ? " " : "", arg);

    char first[16];
    const char *rest;
    first_token(full, first, sizeof(first), &rest);
    if (alias_exists(first)) {
        snprintf(out, out_size, "alias: loop detected\r\n");
        return true;
    }
    k85_shell_run_command(full, out, out_size);
    return true;
}

static void ext_cron(const char *arg, char *out, size_t out_size) {
    char sub[16];
    const char *rest;
    first_token(arg, sub, sizeof(sub), &rest);
    out[0] = 0;

    if (!strcmp(sub, "on")) {
        k85_svc_set_bool("cron", true);
        bool ok = k85_cron_start();
        snprintf(out, out_size, ok ? "Scheduler started (%d jobs)\r\n" : "Scheduler failed to start\r\n",
                 k85_cron_job_count());
    } else if (!strcmp(sub, "off")) {
        k85_svc_set_bool("cron", false);
        k85_cron_stop();
        snprintf(out, out_size, "Scheduler stopped\r\n");
    } else if (!strcmp(sub, "reload")) {
        int n = k85_cron_reload();
        snprintf(out, out_size, "Loaded %d job(s) from /littlefs/crontab\r\n", n);
    } else if (!strcmp(sub, "list")) {
        k85_cron_list(out, out_size);
    } else {
        snprintf(out, out_size,
                 "Scheduler: %s, jobs: %d\r\nUsage: cron on|off|reload|list\r\n"
                 "File: /littlefs/crontab  (min hour dom mon dow command)\r\n",
                 k85_cron_is_running() ? "running" : "stopped", k85_cron_job_count());
    }
}

static void ext_ftp(const char *arg, char *out, size_t out_size) {
    char sub[16];
    const char *rest;
    first_token(arg, sub, sizeof(sub), &rest);
    out[0] = 0;

    if (!strcmp(sub, "start")) {
        switch (k85_ftp_server_start(true)) {
            case K85_FTP_START_OK:
                snprintf(out, out_size, "FTP started: ftp://%s\r\n", k85_wifi_get_ip_str()); break;
            case K85_FTP_START_NO_WIFI:
                snprintf(out, out_size, "ftp: WiFi not connected\r\n"); break;
            case K85_FTP_START_NO_LOGIN:
                snprintf(out, out_size, "ftp: login not set, use: ftppass <user> <password>\r\n"); break;
            case K85_FTP_START_ALREADY_RUNNING:
                snprintf(out, out_size, "ftp: already running\r\n"); break;
            default:
                snprintf(out, out_size, "ftp: failed to start\r\n"); break;
        }
    } else if (!strcmp(sub, "stop")) {
        k85_ftp_server_stop();
        snprintf(out, out_size, "FTP stopped\r\n");
    } else {
        snprintf(out, out_size, "FTP: %s%s%s\r\nLogin: %s\r\nUsage: ftp start|stop\r\n",
                 k85_ftp_server_is_running() ? "running ftp://" : "stopped",
                 k85_ftp_server_is_running() ? k85_wifi_get_ip_str() : "", "",
                 k85_ftp_has_login() ? "set" : "not set (ftppass <user> <password>)");
    }
}

static void ext_ftppass(const char *arg, char *out, size_t out_size) {
    char user[40];
    const char *pass;
    first_token(arg, user, sizeof(user), &pass);
    size_t ul = strlen(user);
    size_t pl = strlen(pass);
    if (ul == 0 || ul > 31 || pl < 8 || pl > 64) {
        snprintf(out, out_size, "usage: ftppass <user> <password>  (password 8-64 chars)\r\n");
        return;
    }
    for (size_t i = 0; i < ul; i++) {
        unsigned char c = (unsigned char)user[i];
        if (c < 33 || c > 126 || c == '=') {
            snprintf(out, out_size, "ftppass: bad user name\r\n");
            return;
        }
    }
    char hx[100];   // K85SEC3: salted PBKDF2 (same scheme as the lock screen)
    k85_lock_hash_password(pass, hx, sizeof(hx));
    if (!hx[0]) { snprintf(out, out_size, "ftppass: hashing failed\r\n"); return; }
    k85_svc_set_str("ftp_user", user);
    k85_svc_set_str("ftp_hash", hx);
    snprintf(out, out_size, "FTP login saved (user: %s)\r\n", user);
}

bool k85_ext_run_command(const char *cmd, const char *arg, char *out, size_t out_size) {
    if (!strcmp(cmd, "echo")) {
        char tmp[192];
        expand_vars(arg, tmp, sizeof(tmp));
        snprintf(out, out_size, "%s\r\n", tmp);
        return true;
    }
    if (!strcmp(cmd, "set"))     { ext_set(arg, out, out_size); return true; }
    if (!strcmp(cmd, "alias"))   { ext_alias(arg, out, out_size); return true; }
    if (!strcmp(cmd, "unalias")) { ext_unalias(arg, out, out_size); return true; }
    if (!strcmp(cmd, "sleep")) {
        int s = atoi(arg);
        if (s < 0) s = 0;
        if (s > 60) s = 60;
        vTaskDelay(pdMS_TO_TICKS(s * 1000));
        out[0] = 0;
        return true;
    }
    if (!strcmp(cmd, "sh"))      { k85_script_run_file(arg, out, out_size); return true; }
    if (!strcmp(cmd, "cron"))    { ext_cron(arg, out, out_size); return true; }
    if (!strcmp(cmd, "ftp"))     { ext_ftp(arg, out, out_size); return true; }
    if (!strcmp(cmd, "ftppass")) { ext_ftppass(arg, out, out_size); return true; }
    if (k85_shell_ext_run(cmd, arg, out, out_size)) return true;
    return try_alias(cmd, arg, out, out_size);
}

// ---------- boot ----------
static void boot_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    k85_screenshot_start();   // A+B short tap = screenshot (also in safe mode)

    // Safe mode: after a crash/watchdog/brownout do not run anything
    // automatically, so a bad script or job can't cause a boot loop.
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
        rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT) {
        ESP_LOGW(TAG, "abnormal reset (%d): automation skipped this boot", (int)rr);
        vTaskDelete(nullptr);
        return;
    }

    if (k85_svc_get_bool("cron", false)) k85_cron_start();

    if (k85_svc_get_bool("startup", true)) {
        struct stat st;
        if (stat(K85_STARTUP_PATH, &st) == 0) {
            ESP_LOGI(TAG, "running %s", K85_STARTUP_PATH);
            k85_script_run_file(K85_STARTUP_PATH, nullptr, 0);
        }
    }

    if (k85_svc_get_bool("ftp_auto", false)) {
        for (int i = 0; i < 45 && !k85_wifi_is_connected(); i++) vTaskDelay(pdMS_TO_TICKS(1000));
        K85FtpStartResult r = k85_ftp_server_start(false);
        ESP_LOGI(TAG, "FTP autostart result: %d", (int)r);
    }
    vTaskDelete(nullptr);
}

void k85_automation_boot(void) {
    xTaskCreate(boot_task, "k85_boot", 6144, nullptr, 3, nullptr);
}

#pragma GCC diagnostic pop
