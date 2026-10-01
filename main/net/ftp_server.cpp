#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "ftp_server.h"
#include "wifi.h"
#include "ssh_server.h"
#include "../core/svc_cfg.h"
#include "../core/fs_guard.h"
#include "../core/cron.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "k85_ftp";

#define FTP_PORT         21
#define FTP_IDLE_STOP_MS (15 * 60 * 1000)
#define FTP_CTL_IDLE_MS  (5 * 60 * 1000)
#define FTP_XFER_BUF     4096

static volatile bool s_run = false;
static bool s_use_idle = false;
static TaskHandle_t s_task = nullptr;
static int s_listen = -1;
static uint32_t s_bound_ip = 0;

struct Sess {
    int ctl;
    int pasv;
    uint32_t peer_ip;
    bool authed;
    bool user_given;
    bool have_rnfr;
    int fails;
    char user[40];
    char cwd[128];
    char rnfr[240];
    char line[256];
    uint8_t buf[FTP_XFER_BUF];
};

static SemaphoreHandle_t state_mtx(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}

struct StateLock {
    StateLock() { xSemaphoreTake(state_mtx(), portMAX_DELAY); }
    ~StateLock() { xSemaphoreGive(state_mtx()); }
};

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------- small helpers ----------
static bool ct_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned d = (la != lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) d |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
    return d == 0;
}

static bool send_all(int fd, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        int n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void reply(Sess *s, int code, const char *text) {
    char m[224];
    int n = snprintf(m, sizeof(m), "%d %s\r\n", code, text);
    if (n > 0) send_all(s->ctl, m, (size_t)n);
}

// Reads one command line into s->line. 1 = ok, 0 = closed, -1 = idle timeout.
static int read_line(Sess *s) {
    size_t n = 0;
    uint32_t last = now_ms();
    while (s_run) {
        char c;
        int r = recv(s->ctl, &c, 1, 0);
        if (r == 1) {
            last = now_ms();
            if (c == '\n') {
                s->line[n] = 0;
                while (n && s->line[n - 1] == '\r') s->line[--n] = 0;
                return 1;
            }
            if (n < sizeof(s->line) - 1) s->line[n++] = c;
            continue;
        }
        if (r == 0) return 0;
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            if (now_ms() - last > FTP_CTL_IDLE_MS) return -1;
            continue;
        }
        if (errno == EINTR) continue;
        return 0;
    }
    return 0;
}

// Virtual path -> normalized absolute virtual path ("/" or "/a/b"). ".." can
// never climb above the root. Returns false for illegal names.
static bool resolve(const char *cwd, const char *arg, char *virt, size_t vsz) {
    char tmp[400];
    if (arg[0] == '/') {
        if (strlen(arg) >= sizeof(tmp)) return false;
        snprintf(tmp, sizeof(tmp), "%s", arg);
    } else {
        if (strlen(cwd) + strlen(arg) + 2 > sizeof(tmp)) return false;
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, arg);
    }

    size_t len = 0;
    size_t marks[16];
    int depth = 0;
    virt[0] = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *q = p;
        while (*q && *q != '/') q++;
        size_t cl = (size_t)(q - p);
        if (cl == 1 && p[0] == '.') {
            // current dir: skip
        } else if (cl == 2 && p[0] == '.' && p[1] == '.') {
            if (depth > 0) { len = marks[--depth]; virt[len] = 0; }
        } else {
            if (depth >= 16 || len + 1 + cl + 1 > vsz) return false;
            for (size_t i = 0; i < cl; i++) {
                unsigned char c = (unsigned char)p[i];
                if (c < 32 || c == 127 || c == '\\') return false;
            }
            marks[depth++] = len;
            virt[len++] = '/';
            memcpy(virt + len, p, cl);
            len += cl;
            virt[len] = 0;
        }
        p = q;
    }
    if (len == 0) { virt[0] = '/'; virt[1] = 0; }
    return true;
}

static void to_full(const char *virt, char *full, size_t n) {
    snprintf(full, n, "/littlefs%s", strcmp(virt, "/") ? virt : "");
}

static bool path_arg(Sess *s, const char *arg, char *virt, size_t vsz, char *full, size_t fsz, bool allow_root) {
    if (!arg[0]) return false;
    if (!resolve(s->cwd, arg, virt, vsz)) return false;
    if (k85_is_blocked_path(virt)) return false;
    if (!allow_root && !strcmp(virt, "/")) return false;
    to_full(virt, full, fsz);
    return true;
}

// ---------- data connection ----------
static void close_pasv(Sess *s) {
    if (s->pasv >= 0) { close(s->pasv); s->pasv = -1; }
}

static int data_accept(Sess *s) {
    if (s->pasv < 0) return -1;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s->pasv, &rf);
    struct timeval tv = {10, 0};
    if (select(s->pasv + 1, &rf, nullptr, nullptr, &tv) <= 0) { close_pasv(s); return -1; }

    struct sockaddr_in pa;
    socklen_t pl = sizeof(pa);
    int d = accept(s->pasv, (struct sockaddr *)&pa, &pl);
    close_pasv(s);
    if (d < 0) return -1;
    // only the client that owns the control connection may use the data port
    if (pa.sin_addr.s_addr != s->peer_ip) { close(d); return -1; }

    struct timeval to = {15, 0};
    setsockopt(d, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    setsockopt(d, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    return d;
}

static void do_pasv(Sess *s, bool ext) {
    close_pasv(s);
    uint32_t ip = k85_wifi_get_ip_addr();
    if (!ip) { reply(s, 425, "No network"); return; }

    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ls < 0) { reply(s, 425, "Can't open data connection"); return; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = 0;
    socklen_t al = sizeof(a);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 1) < 0 ||
        getsockname(ls, (struct sockaddr *)&a, &al) < 0) {
        close(ls);
        reply(s, 425, "Can't open data connection");
        return;
    }
    s->pasv = ls;
    unsigned port = ntohs(a.sin_port);

    char m[96];
    if (ext) {
        snprintf(m, sizeof(m), "Entering Extended Passive Mode (|||%u|)", port);
        reply(s, 229, m);
    } else {
        const uint8_t *b = (const uint8_t *)&ip;
        snprintf(m, sizeof(m), "Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
                 (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3], port >> 8, port & 255);
        reply(s, 227, m);
    }
}

// ---------- commands ----------
static void do_list(Sess *s, const char *arg, bool names_only) {
    while (arg[0] == '-') {                       // ignore flags like -a / -la
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
    }
    char virt[192], full[224];
    if (arg[0]) {
        if (!resolve(s->cwd, arg, virt, sizeof(virt))) { reply(s, 550, "Bad path"); return; }
    } else {
        snprintf(virt, sizeof(virt), "%s", s->cwd);
    }
    if (k85_is_blocked_path(virt)) { reply(s, 550, "Access denied"); return; }
    to_full(virt, full, sizeof(full));

    DIR *d = opendir(full);
    if (!d) { reply(s, 550, "Cannot open directory"); return; }
    reply(s, 150, "Opening data connection");
    int dc = data_accept(s);
    if (dc < 0) { closedir(d); reply(s, 425, "Can't open data connection"); return; }

    size_t used = 0;
    bool ok = true;
    struct dirent *e;
    while (ok && s_run && (e = readdir(d)) != nullptr) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (k85_is_blocked_path(nm)) continue;

        char child[288];
        snprintf(child, sizeof(child), "%s/%s", full, nm);
        struct stat st;
        bool is_dir = false;
        long sz = 0;
        if (stat(child, &st) == 0) { is_dir = S_ISDIR(st.st_mode); sz = (long)st.st_size; }

        char line[352];
        int n;
        if (names_only) n = snprintf(line, sizeof(line), "%s\r\n", nm);
        else n = snprintf(line, sizeof(line), "%s 1 k85 k85 %10ld Jan 01 00:00 %s\r\n",
                          is_dir ? "drwxr-xr-x" : "-rw-r--r--", sz, nm);
        if (n <= 0) continue;
        if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

        if (used + (size_t)n > FTP_XFER_BUF) { ok = send_all(dc, s->buf, used); used = 0; }
        memcpy(s->buf + used, line, (size_t)n);
        used += (size_t)n;
    }
    if (ok && used) ok = send_all(dc, s->buf, used);
    if (!s_run) ok = false;
    closedir(d);
    close(dc);
    if (ok) reply(s, 226, "Directory send OK"); else reply(s, 426, "Transfer aborted");
}

static void do_retr(Sess *s, const char *arg) {
    char virt[192], full[224];
    if (!path_arg(s, arg, virt, sizeof(virt), full, sizeof(full), false)) { reply(s, 550, "Access denied"); return; }
    struct stat st;
    if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) { reply(s, 550, "Not a file"); return; }
    FILE *f = fopen(full, "rb");
    if (!f) { reply(s, 550, "Cannot open file"); return; }

    reply(s, 150, "Opening data connection");
    int dc = data_accept(s);
    if (dc < 0) { fclose(f); reply(s, 425, "Can't open data connection"); return; }

    bool ok = true;
    unsigned iter = 0;
    size_t n;
    while (ok && s_run && (n = fread(s->buf, 1, FTP_XFER_BUF, f)) > 0) {
        ok = send_all(dc, s->buf, n);
        if ((++iter & 15) == 0) vTaskDelay(1);
    }
    if (!s_run) ok = false;
    fclose(f);
    close(dc);
    if (ok) reply(s, 226, "Transfer complete"); else reply(s, 426, "Transfer aborted");
}

static void do_stor(Sess *s, const char *arg) {
    char virt[192], full[224];
    if (!path_arg(s, arg, virt, sizeof(virt), full, sizeof(full), false)) { reply(s, 550, "Access denied"); return; }
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) { reply(s, 550, "Is a directory"); return; }

    // upload into a temp file, move into place only when complete
    char tmp[240];
    snprintf(tmp, sizeof(tmp), "%s.k85tmp", full);
    FILE *f = fopen(tmp, "wb");
    if (!f) { reply(s, 550, "Cannot create file"); return; }

    reply(s, 150, "Ready for data");
    int dc = data_accept(s);
    if (dc < 0) { fclose(f); unlink(tmp); reply(s, 425, "Can't open data connection"); return; }

    bool ok = true;
    unsigned iter = 0;
    while (s_run) {
        int n = recv(dc, s->buf, FTP_XFER_BUF, 0);
        if (n > 0) {
            if (fwrite(s->buf, 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
            if ((++iter & 15) == 0) vTaskDelay(1);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
    }
    if (!s_run) ok = false;
    close(dc);
    if (fclose(f) != 0) ok = false;
    if (ok) {
        unlink(full);
        if (rename(tmp, full) != 0) ok = false;
    }
    if (!ok) unlink(tmp);
    if (ok && !strcmp(virt, "/crontab")) k85_cron_reload();
    if (ok) reply(s, 226, "Upload complete"); else reply(s, 552, "Upload failed");
}

static void do_pass(Sess *s, const char *arg, bool *close_session) {
    char cu[40] = {0}, ch[72] = {0};
    bool have = k85_svc_get_str("ftp_user", cu, sizeof(cu)) && k85_svc_get_str("ftp_hash", ch, sizeof(ch));
    char hx[65];
    k85_ssh_hash_password(arg, hx);
    bool ok = have && s->user_given && ct_eq(cu, s->user) && ct_eq(ch, hx);
    if (ok) {
        s->authed = true;
        reply(s, 230, "Login successful");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));              // slow down guessing
    s->fails++;
    s->authed = false;
    if (s->fails >= 3) {
        reply(s, 421, "Too many failed logins");
        *close_session = true;
    } else {
        reply(s, 530, "Login incorrect");
    }
}

// Returns false to end the session.
static bool handle(Sess *s) {
    char cmd[8];
    size_t k = 0;
    while (s->line[k] && s->line[k] != ' ' && k < sizeof(cmd) - 1) {
        cmd[k] = (char)toupper((unsigned char)s->line[k]);
        k++;
    }
    cmd[k] = 0;
    const char *arg = s->line + k;
    while (*arg && *arg != ' ') arg++;
    while (*arg == ' ') arg++;

    bool pre = !strcmp(cmd, "USER") || !strcmp(cmd, "PASS") || !strcmp(cmd, "QUIT") ||
               !strcmp(cmd, "FEAT") || !strcmp(cmd, "SYST") || !strcmp(cmd, "OPTS") || !strcmp(cmd, "NOOP");
    if (!s->authed && !pre) { reply(s, 530, "Please login with USER and PASS"); return true; }

    char virt[192], full[224], m[224];

    if (!strcmp(cmd, "USER")) {
        if (!arg[0]) { reply(s, 501, "Missing user name"); return true; }
        snprintf(s->user, sizeof(s->user), "%s", arg);
        s->user_given = true;
        s->authed = false;
        reply(s, 331, "Password required");
    } else if (!strcmp(cmd, "PASS")) {
        bool close_it = false;
        do_pass(s, arg, &close_it);
        if (close_it) return false;
    } else if (!strcmp(cmd, "QUIT")) {
        reply(s, 221, "Goodbye");
        return false;
    } else if (!strcmp(cmd, "SYST")) {
        reply(s, 215, "UNIX Type: L8");
    } else if (!strcmp(cmd, "FEAT")) {
        static const char kFeat[] = "211-Features:\r\n SIZE\r\n UTF8\r\n211 End\r\n";
        send_all(s->ctl, kFeat, sizeof(kFeat) - 1);
    } else if (!strcmp(cmd, "OPTS") || !strcmp(cmd, "TYPE") || !strcmp(cmd, "MODE") || !strcmp(cmd, "STRU")) {
        reply(s, 200, "OK");
    } else if (!strcmp(cmd, "NOOP")) {
        reply(s, 200, "OK");
    } else if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD")) {
        snprintf(m, sizeof(m), "\"%s\" is current directory", s->cwd);
        reply(s, 257, m);
    } else if (!strcmp(cmd, "CWD") || !strcmp(cmd, "CDUP")) {
        const char *target = !strcmp(cmd, "CDUP") ? ".." : arg;
        struct stat st;
        if (!path_arg(s, target, virt, sizeof(virt), full, sizeof(full), true) ||
            stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
            reply(s, 550, "Directory not available");
        } else if (strlen(virt) >= sizeof(s->cwd)) {
            reply(s, 550, "Path too long");
        } else {
            snprintf(s->cwd, sizeof(s->cwd), "%s", virt);
            reply(s, 250, "Directory changed");
        }
    } else if (!strcmp(cmd, "PASV")) {
        do_pasv(s, false);
    } else if (!strcmp(cmd, "EPSV")) {
        do_pasv(s, true);
    } else if (!strcmp(cmd, "PORT") || !strcmp(cmd, "EPRT")) {
        reply(s, 502, "Active mode not supported, use passive");
    } else if (!strcmp(cmd, "LIST")) {
        do_list(s, arg, false);
    } else if (!strcmp(cmd, "NLST")) {
        do_list(s, arg, true);
    } else if (!strcmp(cmd, "RETR")) {
        do_retr(s, arg);
    } else if (!strcmp(cmd, "STOR")) {
        do_stor(s, arg);
    } else if (!strcmp(cmd, "SIZE")) {
        struct stat st;
        if (!path_arg(s, arg, virt, sizeof(virt), full, sizeof(full), false) ||
            stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
            reply(s, 550, "Not a file");
        } else {
            snprintf(m, sizeof(m), "%ld", (long)st.st_size);
            reply(s, 213, m);
        }
    } else if (!strcmp(cmd, "DELE")) {
        struct stat st;
        if (!path_arg(s, arg, virt, sizeof(virt), full, sizeof(full), false) ||
            stat(full, &st) != 0 || S_ISDIR(st.st_mode) || unlink(full) != 0) {
            reply(s, 550, "Delete failed");
        } else {
            if (!strcmp(virt, "/crontab")) k85_cron_reload();
            reply(s, 250, "Deleted");
        }
    } else if (!strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD")) {
        if (!path_arg(s, arg, virt, sizeof(virt), full, sizeof(full), false) || mkdir(full, 0755) != 0) {
            reply(s, 550, "Cannot create directory");
        } else {
            snprintf(m, sizeof(m), "\"%s\" created", virt);
            reply(s, 257, m);
        }
    } else if (!strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD")) {
        if (!path_arg(s, arg, virt, sizeof(virt), full, sizeof(full), false) || rmdir(full) != 0) {
            reply(s, 550, "Cannot remove directory");
        } else {
            reply(s, 250, "Removed");
        }
    } else if (!strcmp(cmd, "RNFR")) {
        struct stat st;
        s->have_rnfr = false;
        if (!path_arg(s, arg, virt, sizeof(virt), full, sizeof(full), false) || stat(full, &st) != 0) {
            reply(s, 550, "File not found");
        } else {
            snprintf(s->rnfr, sizeof(s->rnfr), "%s", full);
            s->have_rnfr = true;
            reply(s, 350, "Ready for RNTO");
        }
    } else if (!strcmp(cmd, "RNTO")) {
        bool was = s->have_rnfr;
        s->have_rnfr = false;
        if (!was) {
            reply(s, 503, "Send RNFR first");
        } else if (!path_arg(s, arg, virt, sizeof(virt), full, sizeof(full), false) || rename(s->rnfr, full) != 0) {
            reply(s, 550, "Rename failed");
        } else {
            k85_cron_reload();   // cheap; covers renames to/from "crontab"
            reply(s, 250, "Renamed");
        }
    } else if (!strcmp(cmd, "ABOR")) {
        close_pasv(s);
        reply(s, 226, "OK");
    } else {
        reply(s, 502, "Command not implemented");
    }
    return true;
}

static void session(Sess *s, int ctl, uint32_t peer_ip) {
    memset(s, 0, sizeof(*s));
    s->ctl = ctl;
    s->pasv = -1;
    s->peer_ip = peer_ip;
    snprintf(s->cwd, sizeof(s->cwd), "/");

    struct timeval rt = {1, 0};
    struct timeval st = {10, 0};
    setsockopt(ctl, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
    setsockopt(ctl, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));

    ESP_LOGI(TAG, "client connected");
    reply(s, 220, "k85OS FTP ready");
    while (s_run) {
        int r = read_line(s);
        if (r <= 0) {
            if (r < 0) reply(s, 421, "Timeout");
            break;
        }
        if (!handle(s)) break;
    }
    close_pasv(s);
    shutdown(ctl, SHUT_RDWR);
    close(ctl);
    ESP_LOGI(TAG, "client disconnected");
}

// ---------- server task ----------
static int make_listener(uint32_t ip) {
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ls < 0) return -1;
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;               // STA address only: never reachable via the hotspot
    a.sin_port = htons(FTP_PORT);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 1) < 0) {
        close(ls);
        return -1;
    }
    return ls;
}

static void ftp_task(void *arg) {
    (void)arg;
    Sess *s = (Sess *)heap_caps_calloc(1, sizeof(Sess), MALLOC_CAP_SPIRAM);
    if (!s) s = (Sess *)calloc(1, sizeof(Sess));
    uint32_t last_activity = now_ms();

    while (s_run) {
        // follow DHCP address changes
        uint32_t cur = k85_wifi_get_ip_addr();
        if (cur && cur != s_bound_ip) {
            int nl = make_listener(cur);
            if (nl >= 0) {
                if (s_listen >= 0) close(s_listen);
                s_listen = nl;
                s_bound_ip = cur;
            }
        }

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s_listen, &rf);
        struct timeval tv = {1, 0};
        int r = select(s_listen + 1, &rf, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(s_listen, &rf)) {
            struct sockaddr_in pa;
            socklen_t pl = sizeof(pa);
            int c = accept(s_listen, (struct sockaddr *)&pa, &pl);
            if (c >= 0) {
                if (s) session(s, c, pa.sin_addr.s_addr); else close(c);
            }
            last_activity = now_ms();
        } else if (s_use_idle && now_ms() - last_activity > FTP_IDLE_STOP_MS) {
            ESP_LOGI(TAG, "idle timeout, stopping");
            s_run = false;
        }
    }

    if (s_listen >= 0) { close(s_listen); s_listen = -1; }
    s_bound_ip = 0;
    if (s) heap_caps_free(s);
    ESP_LOGI(TAG, "stopped");
    s_task = nullptr;
    vTaskDelete(nullptr);
}

bool k85_ftp_has_login(void) {
    char u[40], h[72];
    return k85_svc_get_str("ftp_user", u, sizeof(u)) && k85_svc_get_str("ftp_hash", h, sizeof(h));
}

bool k85_ftp_server_is_running(void) {
    return s_run && s_task != nullptr;
}

K85FtpStartResult k85_ftp_server_start(bool idle_timeout) {
    StateLock lk;
    if (s_task) return K85_FTP_START_ALREADY_RUNNING;
    if (!k85_wifi_is_connected()) return K85_FTP_START_NO_WIFI;
    if (!k85_ftp_has_login()) return K85_FTP_START_NO_LOGIN;
    uint32_t ip = k85_wifi_get_ip_addr();
    if (!ip) return K85_FTP_START_NO_WIFI;

    int ls = make_listener(ip);
    if (ls < 0) return K85_FTP_START_FAILED;

    s_listen = ls;
    s_bound_ip = ip;
    s_use_idle = idle_timeout;
    s_run = true;
    if (xTaskCreate(ftp_task, "k85_ftp", 7168, nullptr, 3, &s_task) != pdPASS) {
        close(ls);
        s_listen = -1;
        s_bound_ip = 0;
        s_run = false;
        s_task = nullptr;
        return K85_FTP_START_FAILED;
    }
    ESP_LOGI(TAG, "listening on port %d", FTP_PORT);
    return K85_FTP_START_OK;
}

void k85_ftp_server_stop(void) {
    StateLock lk;
    s_run = false;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
}

#pragma GCC diagnostic pop
