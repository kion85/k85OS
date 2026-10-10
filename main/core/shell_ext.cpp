#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "shell_ext.h"
#include "config.h"
#include "device.h"
#include "version.h"
#include "fs_guard.h"
#include "script_engine.h"
#include "shell_commands.h"
#include "screenshot.h"
#include "../net/app_repo.h"
#include "../net/wifi.h"

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "M5Unified.h"

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef K85_FS_ROOT
#define K85_FS_ROOT "/littlefs"
#endif

#define MAX_ARGS 12

static thread_local const char *tl_stdin = nullptr;   // output of the previous pipeline stage

// ---------- output buffer ----------
struct Out { char *b; size_t cap, len; };

static void o_init(Out *o, char *b, size_t cap) { o->b = b; o->cap = cap; o->len = 0; if (cap) b[0] = 0; }

static void o_puts(Out *o, const char *s) {
    if (!o->cap) return;
    size_t n = strlen(s);
    if (o->len + n + 1 > o->cap) n = (o->cap > o->len + 1) ? o->cap - o->len - 1 : 0;
    memcpy(o->b + o->len, s, n);
    o->len += n;
    o->b[o->len] = 0;
}

static void o_printf(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void o_printf(Out *o, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    o_puts(o, tmp);
}

static char *xalloc(size_t n) {
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    if (!p) p = calloc(1, n);
    return (char *)p;
}

// ---------- parsing helpers ----------
// Splits on blanks; '...' and "..." group words and are removed.
static int tokenize(const char *s, char *buf, size_t bufsz, char *argv[], int max) {
    int argc = 0;
    size_t o = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || argc >= max || o + 1 >= bufsz) break;
        argv[argc++] = buf + o;
        char q = 0;
        while (*s && (q || (*s != ' ' && *s != '\t'))) {
            if (q) {
                if (*s == q) { q = 0; s++; continue; }
            } else if (*s == '\'' || *s == '"') {
                q = *s;
                s++;
                continue;
            }
            if (o + 1 >= bufsz) break;
            buf[o++] = *s++;
        }
        buf[o++] = 0;
    }
    return argc;
}

static bool has_unquoted(const char *s, char c) {
    char q = 0;
    for (; *s; s++) {
        if (q) { if (*s == q) q = 0; }
        else if (*s == '\'' || *s == '"') q = *s;
        else if (*s == c) return true;
    }
    return false;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

static bool ci_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return true;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
    }
    return false;
}

// ---------- paths (all relative to /littlefs, same as ls/cat) ----------
static bool fs_path(const char *arg, char *full, size_t n, bool allow_root) {
    if (!arg || !arg[0]) return false;
    const char *p = arg;
    size_t rl = strlen(K85_FS_ROOT);
    if (!strncmp(p, K85_FS_ROOT, rl) && (p[rl] == '/' || p[rl] == 0)) p += rl;
    while (*p == '/') p++;
    if (strstr(p, "..")) return false;
    if (!*p && !allow_root) return false;
    if (*p) snprintf(full, n, "%s/%s", K85_FS_ROOT, p);
    else snprintf(full, n, "%s", K85_FS_ROOT);
    size_t l = strlen(full);
    while (l > rl && full[l - 1] == '/') full[--l] = 0;
    return !k85_is_blocked_path(full);
}

static bool path_arg(Out *o, const char *cmd, const char *arg, char *full, size_t n, bool allow_root) {
    if (fs_path(arg, full, n, allow_root)) return true;
    o_printf(o, "%s: path not allowed: %s\r\n", cmd, arg ? arg : "");
    return false;
}

static bool is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void mkdirs(const char *full) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", full);
    size_t rl = strlen(K85_FS_ROOT);
    for (size_t i = rl + 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

// ---------- line source: a file or the previous pipeline stage ----------
struct LineSrc {
    FILE *f = nullptr;
    const char *p = nullptr;

    bool next(char *line, size_t n) {
        if (f) {
            if (!fgets(line, (int)n, f)) return false;
            size_t l = strlen(line);
            if (l && line[l - 1] == '\n') {
                line[--l] = 0;
            } else if (!feof(f)) {
                int c;
                while ((c = fgetc(f)) != EOF && c != '\n') {}
            }
            if (l && line[l - 1] == '\r') line[l - 1] = 0;
            return true;
        }
        if (p && *p) {
            size_t i = 0;
            while (*p && *p != '\n') { if (i + 1 < n) line[i++] = *p; p++; }
            if (*p == '\n') p++;
            while (i && line[i - 1] == '\r') i--;
            line[i] = 0;
            return true;
        }
        return false;
    }
    void close() { if (f) { fclose(f); f = nullptr; } }
};

static bool open_src(LineSrc *s, const char *file, Out *o, const char *cmd) {
    if (file) {
        char full[200];
        if (!path_arg(o, cmd, file, full, sizeof(full), false)) return false;
        s->f = fopen(full, "r");
        if (!s->f) { o_printf(o, "%s: cannot open %s\r\n", cmd, file); return false; }
        return true;
    }
    if (tl_stdin) { s->p = tl_stdin; return true; }
    o_printf(o, "%s: no input (give a file or use a pipe)\r\n", cmd);
    return false;
}

// parses "-n N" / "-N" at argv[*i]; returns true if consumed
static bool opt_count(int *i, int argc, char **argv, int *n) {
    if (argv[*i][0] != '-') return false;
    if (!strcmp(argv[*i], "-n") && *i + 1 < argc) { *n = atoi(argv[*i + 1]); *i += 2; return true; }
    if (argv[*i][1] >= '0' && argv[*i][1] <= '9') { *n = atoi(argv[*i] + 1); (*i)++; return true; }
    return false;
}

// ---------- text filters ----------
static void cmd_grep(int argc, char **argv, Out *o) {
    bool icase = false, invert = false, count = false, num = false;
    int i = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'i') icase = true;
            else if (*f == 'v') invert = true;
            else if (*f == 'c') count = true;
            else if (*f == 'n') num = true;
            else { o_printf(o, "grep: unknown option -%c\r\n", *f); return; }
        }
    }
    if (i >= argc) { o_puts(o, "usage: grep [-ivcn] text [file]   (plain text, not regex)\r\n"); return; }
    const char *pat = argv[i++];
    LineSrc src;
    if (!open_src(&src, i < argc ? argv[i] : nullptr, o, "grep")) return;
    char line[256];
    int n = 0, hits = 0;
    while (src.next(line, sizeof(line))) {
        n++;
        bool hit = icase ? ci_contains(line, pat) : strstr(line, pat) != nullptr;
        if (hit == invert) continue;
        hits++;
        if (count) continue;
        if (num) o_printf(o, "%d:%s\r\n", n, line);
        else o_printf(o, "%s\r\n", line);
    }
    if (count) o_printf(o, "%d\r\n", hits);
    src.close();
}

static void cmd_head(int argc, char **argv, Out *o) {
    int n = 10, i = 0;
    while (i < argc && opt_count(&i, argc, argv, &n)) {}
    LineSrc src;
    if (!open_src(&src, i < argc ? argv[i] : nullptr, o, "head")) return;
    char line[256];
    for (int k = 0; k < n && src.next(line, sizeof(line)); k++) o_printf(o, "%s\r\n", line);
    src.close();
}

static void cmd_tail(int argc, char **argv, Out *o) {
    int n = 10, i = 0;
    while (i < argc && opt_count(&i, argc, argv, &n)) {}
    if (n < 1) n = 1;
    if (n > 50) n = 50;
    LineSrc src;
    if (!open_src(&src, i < argc ? argv[i] : nullptr, o, "tail")) return;
    char (*ring)[160] = (char (*)[160])xalloc((size_t)n * 160);
    if (!ring) { o_puts(o, "tail: out of memory\r\n"); src.close(); return; }
    char line[256];
    long total = 0;
    while (src.next(line, sizeof(line))) {
        snprintf(ring[total % n], 160, "%s", line);
        total++;
    }
    long start = total > n ? total - n : 0;
    for (long k = start; k < total; k++) o_printf(o, "%s\r\n", ring[k % n]);
    free(ring);
    src.close();
}

static void cmd_wc(int argc, char **argv, Out *o) {
    bool l = false, w = false, c = false;
    int i = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'l') l = true; else if (*f == 'w') w = true; else if (*f == 'c') c = true;
            else { o_printf(o, "wc: unknown option -%c\r\n", *f); return; }
        }
    }
    if (!l && !w && !c) l = w = c = true;
    LineSrc src;
    if (!open_src(&src, i < argc ? argv[i] : nullptr, o, "wc")) return;
    char line[256];
    long lines = 0, words = 0, bytes = 0;
    while (src.next(line, sizeof(line))) {
        lines++;
        bytes += (long)strlen(line) + 1;
        bool in = false;
        for (const char *p = line; *p; p++) {
            if (*p == ' ' || *p == '\t') in = false;
            else if (!in) { in = true; words++; }
        }
    }
    src.close();
    if (l) o_printf(o, "%ld ", lines);
    if (w) o_printf(o, "%ld ", words);
    if (c) o_printf(o, "%ld ", bytes);
    o_puts(o, "\r\n");
}

// ---------- files ----------
static bool copy_one(const char *src, const char *dst, Out *o, const char *cmd) {
    FILE *in = fopen(src, "rb");
    if (!in) { o_printf(o, "%s: cannot open source\r\n", cmd); return false; }
    char *buf = xalloc(2048);
    FILE *out = buf ? fopen(dst, "wb") : nullptr;
    if (!out) { if (in) fclose(in); free(buf); o_printf(o, "%s: cannot create destination\r\n", cmd); return false; }
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, 2048, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    free(buf);
    if (!ok) { unlink(dst); o_printf(o, "%s: write error (disk full?)\r\n", cmd); }
    return ok;
}

// dst may be a directory: then the source name is kept
static bool resolve_dst(const char *src_full, const char *dst_arg, char *dst_full, size_t n, Out *o, const char *cmd) {
    if (!path_arg(o, cmd, dst_arg, dst_full, n, true)) return false;
    if (is_dir(dst_full)) {
        size_t l = strlen(dst_full);
        snprintf(dst_full + l, n - l, "/%s", base_name(src_full));
        if (k85_is_blocked_path(dst_full)) { o_printf(o, "%s: path not allowed\r\n", cmd); return false; }
    }
    return true;
}

static void cmd_cp(int argc, char **argv, Out *o) {
    if (argc != 2) { o_puts(o, "usage: cp <src> <dst>\r\n"); return; }
    char s[200], d[200];
    if (!path_arg(o, "cp", argv[0], s, sizeof(s), false)) return;
    if (is_dir(s)) { o_puts(o, "cp: source is a directory (use tar)\r\n"); return; }
    if (!resolve_dst(s, argv[1], d, sizeof(d), o, "cp")) return;
    if (!strcmp(s, d)) { o_puts(o, "cp: source and destination are the same\r\n"); return; }
    copy_one(s, d, o, "cp");
}

static void cmd_mv(int argc, char **argv, Out *o) {
    if (argc != 2) { o_puts(o, "usage: mv <src> <dst>\r\n"); return; }
    char s[200], d[200];
    if (!path_arg(o, "mv", argv[0], s, sizeof(s), false)) return;
    if (!exists(s)) { o_puts(o, "mv: no such file\r\n"); return; }
    if (!resolve_dst(s, argv[1], d, sizeof(d), o, "mv")) return;
    if (!strcmp(s, d)) return;
    if (!is_dir(d)) unlink(d);
    if (rename(s, d) != 0) o_puts(o, "mv: failed\r\n");
}

static int rm_rec(const char *path, int depth, int *skipped) {
    if (!is_dir(path)) return unlink(path);
    if (depth > 6) return -1;
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[240];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (k85_is_blocked_path(child)) { (*skipped)++; continue; }
        if (rm_rec(child, depth + 1, skipped) != 0) rc = -1;
    }
    closedir(d);
    if (*skipped == 0 && rmdir(path) != 0) rc = -1;
    return rc;
}

static void cmd_rm(int argc, char **argv, Out *o) {
    bool rec = false;
    int i = 0;
    if (i < argc && (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-rf") || !strcmp(argv[i], "-R"))) { rec = true; i++; }
    if (i >= argc) { o_puts(o, "usage: rm [-r] <path>...\r\n"); return; }
    for (; i < argc; i++) {
        char full[200];
        if (!path_arg(o, "rm", argv[i], full, sizeof(full), false)) continue;
        if (!exists(full)) { o_printf(o, "rm: %s: no such file\r\n", argv[i]); continue; }
        if (is_dir(full)) {
            if (!rec) { o_printf(o, "rm: %s is a directory (use rm -r)\r\n", argv[i]); continue; }
            int skipped = 0;
            if (rm_rec(full, 0, &skipped) != 0 || skipped) o_printf(o, "rm: %s: not everything removed\r\n", argv[i]);
        } else if (unlink(full) != 0) {
            o_printf(o, "rm: %s: failed\r\n", argv[i]);
        }
    }
}

static void cmd_mkdir(int argc, char **argv, Out *o) {
    bool parents = false;
    int i = 0;
    if (i < argc && !strcmp(argv[i], "-p")) { parents = true; i++; }
    if (i >= argc) { o_puts(o, "usage: mkdir [-p] <dir>\r\n"); return; }
    for (; i < argc; i++) {
        char full[200];
        if (!path_arg(o, "mkdir", argv[i], full, sizeof(full), false)) continue;
        if (parents) { mkdirs(full); continue; }
        if (mkdir(full, 0755) != 0) o_printf(o, "mkdir: cannot create %s\r\n", argv[i]);
    }
}

static void cmd_touch(int argc, char **argv, Out *o) {
    if (argc < 1) { o_puts(o, "usage: touch <file>...\r\n"); return; }
    for (int i = 0; i < argc; i++) {
        char full[200];
        if (!path_arg(o, "touch", argv[i], full, sizeof(full), false)) continue;
        FILE *f = fopen(full, "ab");
        if (!f) { o_printf(o, "touch: cannot create %s\r\n", argv[i]); continue; }
        fclose(f);
    }
}

// ---------- system info ----------
static void cmd_date(int, char **, Out *o) {
    time_t t = time(nullptr);
    if (t < 1700000000) { o_puts(o, "Time not set yet (waiting for NTP, needs WiFi)\r\n"); return; }
    time_t local = t + (time_t)g_config.utc_offset * 3600;
    struct tm tm;
    gmtime_r(&local, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Y", &tm);
    o_printf(o, "%s  (UTC%+d)\r\n", buf, g_config.utc_offset);
}

static void cmd_df(int, char **, Out *o) {
    size_t t = 0, u = 0;
    o_puts(o, "Filesystem    Size    Used   Avail  Use%\r\n");
    if (k85_fs_info(&t, &u) && t) {
        o_printf(o, "littlefs  %6uK %6uK %6uK  %3u%%\r\n", (unsigned)(t / 1024), (unsigned)(u / 1024),
                 (unsigned)((t - u) / 1024), (unsigned)(u * 100 / t));
    }
    size_t it = heap_caps_get_total_size(MALLOC_CAP_INTERNAL), ifr = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (it) o_printf(o, "RAM       %6uK %6uK %6uK  %3u%%\r\n", (unsigned)(it / 1024), (unsigned)((it - ifr) / 1024),
                     (unsigned)(ifr / 1024), (unsigned)((it - ifr) * 100 / it));
    size_t pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM), pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (pt) o_printf(o, "PSRAM     %6uK %6uK %6uK  %3u%%\r\n", (unsigned)(pt / 1024), (unsigned)((pt - pf) / 1024),
                     (unsigned)(pf / 1024), (unsigned)((pt - pf) * 100 / pt));
}

static long du_rec(const char *path, int depth) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (long)st.st_size;
    if (depth > 6) return 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    long sum = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[240];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (k85_is_blocked_path(child)) continue;
        sum += du_rec(child, depth + 1);
    }
    closedir(d);
    return sum;
}

static void cmd_du(int argc, char **argv, Out *o) {
    char full[200];
    if (!path_arg(o, "du", argc ? argv[0] : "/", full, sizeof(full), true)) return;
    if (!is_dir(full)) { o_printf(o, "%ld\t%s\r\n", (du_rec(full, 0) + 1023) / 1024, argc ? argv[0] : "/"); return; }
    DIR *d = opendir(full);
    if (!d) { o_puts(o, "du: cannot open\r\n"); return; }
    struct dirent *e;
    int shown = 0;
    while ((e = readdir(d)) != nullptr && shown < 40) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[300];
        snprintf(child, sizeof(child), "%s/%s", full, e->d_name);
        if (k85_is_blocked_path(child)) continue;
        o_printf(o, "%ld\t%s\r\n", (du_rec(child, 0) + 1023) / 1024, e->d_name);
        shown++;
    }
    closedir(d);
    o_printf(o, "%ld\ttotal (KB)\r\n", (du_rec(full, 0) + 1023) / 1024);
}

static void cmd_hostname(int, char **, Out *o) {
    const char *h = nullptr;
    esp_netif_t *hn = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (hn) esp_netif_get_hostname(hn, &h);
    o_printf(o, "%s\r\n", h && h[0] ? h : "k85os");
}

static void cmd_ifconfig(int, char **, Out *o) {
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!k85_wifi_is_connected() || !n || esp_netif_get_ip_info(n, &ip) != ESP_OK) {
        o_puts(o, "wlan0: not connected\r\n");
        return;
    }
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char a[20], m[20], g[20];
    snprintf(a, sizeof(a), IPSTR, IP2STR(&ip.ip));
    snprintf(m, sizeof(m), IPSTR, IP2STR(&ip.netmask));
    snprintf(g, sizeof(g), IPSTR, IP2STR(&ip.gw));
    o_printf(o, "wlan0: inet %s  netmask %s  gateway %s\r\n", a, m, g);
    o_printf(o, "       ether %02x:%02x:%02x:%02x:%02x:%02x\r\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    o_printf(o, "       ssid %s  rssi %d dBm\r\n", g_config.wifi_ssid, k85_wifi_get_rssi());
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(n, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        char d[20];
        snprintf(d, sizeof(d), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        o_printf(o, "       dns %s\r\n", d);
    }
}

static void cmd_nslookup(int argc, char **argv, Out *o) {
    if (argc < 1) { o_puts(o, "usage: nslookup <host>\r\n"); return; }
    if (!k85_wifi_is_connected()) { o_puts(o, "nslookup: WiFi not connected\r\n"); return; }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(argv[0], nullptr, &hints, &res) != 0 || !res) {
        o_printf(o, "nslookup: cannot resolve %s\r\n", argv[0]);
        return;
    }
    int cnt = 0;
    for (struct addrinfo *p = res; p && cnt < 4; p = p->ai_next, cnt++) {
        char ip[20];
        struct sockaddr_in *sa = (struct sockaddr_in *)p->ai_addr;
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        o_printf(o, "%s has address %s\r\n", argv[0], ip);
    }
    freeaddrinfo(res);
}

// ---------- downloads ----------
static bool fetch_to(const char *url, const char *dest_full, Out *o, const char *cmd) {
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) { o_printf(o, "%s: only http:// and https:// URLs\r\n", cmd); return false; }
    if (!k85_wifi_is_connected()) { o_printf(o, "%s: WiFi not connected\r\n", cmd); return false; }
    char part[260];
    snprintf(part, sizeof(part), "%s.part", dest_full);
    unlink(part);
    if (!k85_apprepo_download_file(url, part)) { unlink(part); o_printf(o, "%s: download failed\r\n", cmd); return false; }
    struct stat st;
    if (stat(part, &st) != 0 || st.st_size == 0) { unlink(part); o_printf(o, "%s: empty download\r\n", cmd); return false; }
    unlink(dest_full);
    if (rename(part, dest_full) != 0) { unlink(part); o_printf(o, "%s: cannot save file\r\n", cmd); return false; }
    return true;
}

static void cmd_wget(int argc, char **argv, Out *o) {
    if (argc < 1) { o_puts(o, "usage: wget <url> [file]\r\n"); return; }
    char name[96];
    if (argc >= 2) {
        snprintf(name, sizeof(name), "%s", argv[1]);
    } else {
        snprintf(name, sizeof(name), "%s", base_name(argv[0]));
        char *q = strpbrk(name, "?#");
        if (q) *q = 0;
        if (!name[0] || strstr(argv[0], "://") == nullptr) snprintf(name, sizeof(name), "download.bin");
    }
    char full[200];
    if (!path_arg(o, "wget", name, full, sizeof(full), false)) return;
    if (is_dir(full)) { o_puts(o, "wget: destination is a directory\r\n"); return; }
    if (!fetch_to(argv[0], full, o, "wget")) return;
    struct stat st;
    stat(full, &st);
    o_printf(o, "saved %s (%ld bytes)\r\n", name, (long)st.st_size);
}

static void cmd_curl(int argc, char **argv, Out *o) {
    if (argc >= 3 && !strcmp(argv[0], "-o")) {
        char *a2[2] = { argv[2], argv[1] };
        cmd_wget(2, a2, o);
        return;
    }
    if (argc < 1) { o_puts(o, "usage: curl <url>   |   curl -o <file> <url>\r\n"); return; }
    char full[200];
    snprintf(full, sizeof(full), "%s/k85_curl.tmp", K85_FS_ROOT);
    if (!fetch_to(argv[0], full, o, "curl")) return;
    FILE *f = fopen(full, "rb");
    if (f) {
        size_t room = o->cap > o->len + 3 ? o->cap - o->len - 3 : 0;
        size_t r = room ? fread(o->b + o->len, 1, room, f) : 0;
        o->len += r;
        o->b[o->len] = 0;
        fclose(f);
    }
    unlink(full);
    o_puts(o, "\r\n");
}

// ---------- watch / neofetch / probes ----------
static void cmd_watch(int argc, char **argv, Out *o) {
    int sec = 1, count = 3, i = 0;
    while (i + 1 < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-n")) sec = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-c")) count = atoi(argv[i + 1]);
        else break;
        i += 2;
    }
    if (sec < 1) sec = 1;
    if (sec > 5) sec = 5;
    if (count < 1) count = 1;
    if (count > 10) count = 10;
    if (i >= argc) { o_puts(o, "usage: watch [-n sec(1-5)] [-c count(1-10)] <command>\r\n"); return; }
    char sub[200] = {0};
    for (int k = i; k < argc; k++) {
        size_t l = strlen(sub);
        snprintf(sub + l, sizeof(sub) - l, "%s%s", k > i ? " " : "", argv[k]);
    }
    char *tmp = xalloc(o->cap ? o->cap : 1);
    if (!tmp) { o_puts(o, "watch: out of memory\r\n"); return; }
    for (int k = 1; k <= count; k++) {
        o_printf(o, "--- %d/%d: %s ---\r\n", k, count, sub);
        tmp[0] = 0;
        k85_shell_run_command(sub, tmp, o->cap);
        o_puts(o, tmp);
        if (k < count) vTaskDelay(pdMS_TO_TICKS(sec * 1000));
    }
    free(tmp);
}

static void cmd_neofetch(int, char **, Out *o) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    int64_t s = esp_timer_get_time() / 1000000;
    size_t it = heap_caps_get_total_size(MALLOC_CAP_INTERNAL), ifr = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM), pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ft = 0, fu = 0;
    k85_fs_info(&ft, &fu);

    o_puts(o, "root@k85os\r\n----------\r\n");
    o_printf(o, "OS:      k85OS v%s\r\n", K85_FW_VERSION);
    o_printf(o, "Host:    %s\r\n", k85_get_device_name());
    o_printf(o, "Kernel:  ESP-IDF %s\r\n", esp_get_idf_version());
    o_printf(o, "Uptime:  %lldh %lldm %llds\r\n", (long long)(s / 3600), (long long)((s % 3600) / 60), (long long)(s % 60));
    o_printf(o, "CPU:     %s rev %d.%d, %d cores\r\n", CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100, chip.cores);
    o_printf(o, "Memory:  %uK / %uK  (PSRAM %uK / %uK)\r\n", (unsigned)((it - ifr) / 1024), (unsigned)(it / 1024),
             (unsigned)((pt - pf) / 1024), (unsigned)(pt / 1024));
    o_printf(o, "Disk:    %uK / %uK (littlefs)\r\n", (unsigned)(fu / 1024), (unsigned)(ft / 1024));
    o_printf(o, "Display: %dx%d\r\n", (int)M5.Display.width(), (int)M5.Display.height());
    o_printf(o, "Network: %s\r\n", k85_wifi_is_connected() ? k85_wifi_get_ip_str() : "offline");
    o_puts(o, "Shell:   k85sh\r\n");
}

// Tells whether the display can be read back (needed for screenshots). Leaves a 2x2 blue dot at the top-left.
static void cmd_scrprobe(int, char **, Out *o) {
    auto &d = M5.Display;
    uint16_t red[4] = {0}, blue[4] = {0};
    d.fillRect(0, 0, 2, 2, (uint32_t)0xFF0000);
    d.readRect(0, 0, 2, 2, red);
    d.fillRect(0, 0, 2, 2, (uint32_t)0x0000FF);
    d.readRect(0, 0, 2, 2, blue);
    bool readable = (red[0] != blue[0]);
    o_printf(o, "readback: red=0x%04X blue=0x%04X -> %s\r\n", red[0], blue[0],
             readable ? "display is READABLE (screenshots possible)" : "display is NOT readable");
}

// ---------- at: one-shot jobs ----------
#define AT_MAX 8
struct AtJob { bool used; int id; int64_t due_us; char cmd[96]; };
static AtJob s_at[AT_MAX];
static int s_at_next = 1;
static TaskHandle_t s_at_task = nullptr;

static SemaphoreHandle_t at_mtx(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}

struct AtLock {
    AtLock() { xSemaphoreTake(at_mtx(), portMAX_DELAY); }
    ~AtLock() { xSemaphoreGive(at_mtx()); }
};

static void at_task(void *) {
    char *buf = xalloc(1024);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        char cmd[96];
        bool run = false;
        {
            AtLock lk;
            int64_t now = esp_timer_get_time();
            for (int i = 0; i < AT_MAX; i++) {
                if (s_at[i].used && s_at[i].due_us <= now) {
                    snprintf(cmd, sizeof(cmd), "%s", s_at[i].cmd);
                    s_at[i].used = false;
                    run = true;
                    break;
                }
            }
        }
        if (run && buf) { buf[0] = 0; k85_script_exec(cmd, buf, 1024); }
        AtLock lk;
        bool any = false;
        for (int i = 0; i < AT_MAX; i++) if (s_at[i].used) any = true;
        if (!any) { s_at_task = nullptr; break; }
    }
    free(buf);
    vTaskDelete(nullptr);
}

static void cmd_at(int argc, char **argv, Out *o) {
    if (argc >= 1 && !strcmp(argv[0], "-l")) {
        AtLock lk;
        int64_t now = esp_timer_get_time();
        bool any = false;
        for (int i = 0; i < AT_MAX; i++) {
            if (!s_at[i].used) continue;
            any = true;
            long left = (long)((s_at[i].due_us - now) / 1000000);
            o_printf(o, "%d  in %ldm %lds  %s\r\n", s_at[i].id, left / 60, left % 60, s_at[i].cmd);
        }
        if (!any) o_puts(o, "(no scheduled jobs)\r\n");
        return;
    }
    if (argc >= 2 && !strcmp(argv[0], "-d")) {
        int id = atoi(argv[1]);
        AtLock lk;
        for (int i = 0; i < AT_MAX; i++) {
            if (s_at[i].used && s_at[i].id == id) { s_at[i].used = false; o_printf(o, "job %d removed\r\n", id); return; }
        }
        o_printf(o, "at: no job %d\r\n", id);
        return;
    }
    if (argc < 2) { o_puts(o, "usage: at <N>s|<N>m|<N>h <command>   |   at -l   |   at -d <id>\r\n"); return; }

    char *end = nullptr;
    long n = strtol(argv[0], &end, 10);
    long mult = 0;
    if (end && end != argv[0] && end[1] == 0) {
        if (*end == 's') mult = 1; else if (*end == 'm') mult = 60; else if (*end == 'h') mult = 3600;
    }
    if (n < 1 || !mult || n * mult > 86400) { o_puts(o, "at: time like 30s, 5m, 2h (max 24h)\r\n"); return; }

    char cmd[96] = {0};
    for (int k = 1; k < argc; k++) {
        size_t l = strlen(cmd);
        snprintf(cmd + l, sizeof(cmd) - l, "%s%s", k > 1 ? " " : "", argv[k]);
    }
    AtLock lk;
    int slot = -1;
    for (int i = 0; i < AT_MAX; i++) if (!s_at[i].used) { slot = i; break; }
    if (slot < 0) { o_puts(o, "at: too many jobs (max 8)\r\n"); return; }
    s_at[slot].used = true;
    s_at[slot].id = s_at_next++;
    s_at[slot].due_us = esp_timer_get_time() + (int64_t)n * mult * 1000000;
    snprintf(s_at[slot].cmd, sizeof(s_at[slot].cmd), "%s", cmd);
    if (!s_at_task) {
        if (xTaskCreate(at_task, "k85_at", 5120, nullptr, 3, &s_at_task) != pdPASS) {
            s_at_task = nullptr;
            s_at[slot].used = false;
            o_puts(o, "at: cannot start timer task\r\n");
            return;
        }
    }
    o_printf(o, "job %d in %s: %s   (jobs are lost on reboot)\r\n", s_at[slot].id, argv[0], cmd);
}

// ---------- tar (plain ustar, files and directories) ----------
#define TAR_MAX_ENTRIES 200

static void tar_octal(char *dst, size_t n, uint32_t v) { snprintf(dst, n, "%0*o", (int)(n - 1), (unsigned)v); }

static bool tar_write_header(FILE *f, const char *name, uint32_t size, bool dir, uint32_t mtime) {
    uint8_t h[512];
    memset(h, 0, sizeof(h));
    snprintf((char *)h, 100, "%s", name);
    tar_octal((char *)h + 100, 8, dir ? 0755 : 0644);
    tar_octal((char *)h + 108, 8, 0);
    tar_octal((char *)h + 116, 8, 0);
    tar_octal((char *)h + 124, 12, dir ? 0 : size);
    tar_octal((char *)h + 136, 12, mtime);
    memset(h + 148, ' ', 8);
    h[156] = dir ? '5' : '0';
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 7, "%06o", sum);
    h[154] = 0;
    h[155] = ' ';
    return fwrite(h, 1, 512, f) == 512;
}

struct TarCtx { FILE *f; Out *o; int entries; bool ok; const char *skip_full; };

static void tar_add_file(TarCtx *c, const char *full, const char *name, const struct stat *st) {
    FILE *in = fopen(full, "rb");
    if (!in) { o_printf(c->o, "tar: cannot read %s\r\n", name); return; }
    uint8_t *blk = (uint8_t *)xalloc(512);
    if (!blk) { fclose(in); c->ok = false; return; }
    if (!tar_write_header(c->f, name, (uint32_t)st->st_size, false, (uint32_t)st->st_mtime)) { free(blk); fclose(in); c->ok = false; return; }
    uint32_t left = (uint32_t)st->st_size;
    while (left > 0) {
        size_t want = left < 512 ? left : 512;
        memset(blk, 0, 512);
        if (fread(blk, 1, want, in) != want) { c->ok = false; break; }
        if (fwrite(blk, 1, 512, c->f) != 512) { c->ok = false; break; }
        left -= (uint32_t)want;
    }
    free(blk);
    fclose(in);
    c->entries++;
}

static void tar_add(TarCtx *c, const char *full, const char *name, int depth) {
    if (!c->ok) return;
    if (strlen(name) > 99) { o_printf(c->o, "tar: name too long, skipped: %s\r\n", name); return; }
    if (k85_is_blocked_path(full)) return;
    if (c->skip_full && !strcmp(full, c->skip_full)) return;     // never put the archive into itself
    if (c->entries >= TAR_MAX_ENTRIES) { o_puts(c->o, "tar: too many entries (limit 200)\r\n"); c->ok = false; return; }
    struct stat st;
    if (stat(full, &st) != 0) { o_printf(c->o, "tar: cannot stat %s\r\n", name); return; }
    if (!S_ISDIR(st.st_mode)) { tar_add_file(c, full, name, &st); return; }

    if (depth > 6) return;
    char dname[110];
    snprintf(dname, sizeof(dname), "%s/", name);
    if (!tar_write_header(c->f, dname, 0, true, (uint32_t)st.st_mtime)) { c->ok = false; return; }
    c->entries++;
    DIR *d = opendir(full);
    if (!d) return;
    char *paths = xalloc(460);                         // child path + child name, kept off the stack
    if (!paths) { closedir(d); c->ok = false; return; }
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && c->ok) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(paths, 300, "%s/%s", full, e->d_name);
        snprintf(paths + 300, 160, "%s/%s", name, e->d_name);
        tar_add(c, paths, paths + 300, depth + 1);
    }
    free(paths);
    closedir(d);
}

static bool tar_name_ok(const char *name) {
    if (!name[0] || name[0] == '/' || strstr(name, "..")) return false;
    return strlen(name) < 120;
}

static uint32_t tar_parse_octal(const uint8_t *p, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0');
    return v;
}

static void cmd_tar(int argc, char **argv, Out *o) {
    if (argc < 2) { o_puts(o, "usage: tar -c <archive.tar> <files...>  |  tar -t <archive.tar>  |  tar -x <archive.tar> [dir]\r\n"); return; }
    const char *mode = argv[0];
    char arc[200];
    if (!path_arg(o, "tar", argv[1], arc, sizeof(arc), false)) return;

    if (!strcmp(mode, "-c")) {
        if (argc < 3) { o_puts(o, "tar: nothing to add\r\n"); return; }
        FILE *f = fopen(arc, "wb");
        if (!f) { o_puts(o, "tar: cannot create archive\r\n"); return; }
        TarCtx c = { f, o, 0, true, arc };
        for (int i = 2; i < argc && c.ok; i++) {
            char full[200];
            if (!path_arg(o, "tar", argv[i], full, sizeof(full), true)) continue;
            const char *nm = argv[i];
            while (*nm == '/') nm++;
            if (!strncmp(nm, "littlefs/", 9)) nm += 9;
            if (!*nm) nm = ".";
            if (!strcmp(nm, ".")) {              // whole filesystem root: add its entries
                DIR *d = opendir(full);
                if (!d) continue;
                struct dirent *e;
                while ((e = readdir(d)) != nullptr && c.ok) {
                    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                    char cf[300];
                    snprintf(cf, sizeof(cf), "%s/%s", full, e->d_name);
                    tar_add(&c, cf, e->d_name, 0);
                }
                closedir(d);
            } else {
                tar_add(&c, full, nm, 0);
            }
        }
        uint8_t zero[512] = {0};
        if (c.ok) { c.ok = fwrite(zero, 1, 512, f) == 512 && fwrite(zero, 1, 512, f) == 512; }
        if (fclose(f) != 0) c.ok = false;
        if (!c.ok) { unlink(arc); o_puts(o, "tar: failed (disk full?), archive removed\r\n"); return; }
        o_printf(o, "%s: %d entries\r\n", argv[1], c.entries);
        return;
    }

    bool extract = !strcmp(mode, "-x");
    if (!extract && strcmp(mode, "-t")) { o_puts(o, "tar: use -c, -t or -x\r\n"); return; }
    FILE *f = fopen(arc, "rb");
    if (!f) { o_puts(o, "tar: cannot open archive\r\n"); return; }
    char destdir[200] = {0};
    if (extract && argc >= 3) {
        if (!path_arg(o, "tar", argv[2], destdir, sizeof(destdir), true)) { fclose(f); return; }
        mkdirs(destdir);
    } else {
        snprintf(destdir, sizeof(destdir), "%s", K85_FS_ROOT);
    }

    uint8_t h[512];
    uint8_t *buf = (uint8_t *)xalloc(512);
    int count = 0;
    bool bad = false;
    while (buf && fread(h, 1, 512, f) == 512) {
        bool all_zero = true;
        for (int i = 0; i < 512 && all_zero; i++) if (h[i]) all_zero = false;
        if (all_zero) break;
        unsigned stored = tar_parse_octal(h + 148, 8), sum = 0;
        for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
        if (stored != sum) { o_puts(o, "tar: bad header checksum, stopping\r\n"); bad = true; break; }

        char name[120];
        snprintf(name, sizeof(name), "%.100s", (const char *)h);
        uint32_t size = tar_parse_octal(h + 124, 12);
        char type = (char)h[156];
        bool is_d = (type == '5');
        size_t nl = strlen(name);
        if (is_d && nl && name[nl - 1] == '/') name[nl - 1] = 0;
        uint32_t blocks = (size + 511) / 512;

        if (!extract) {
            o_printf(o, "%s%s  %u\r\n", name, is_d ? "/" : "", (unsigned)(is_d ? 0 : size));
            if (fseek(f, (long)blocks * 512, SEEK_CUR) != 0) break;
            count++;
            continue;
        }

        char full[300];
        snprintf(full, sizeof(full), "%s/%s", destdir, name);
        if (!tar_name_ok(name) || k85_is_blocked_path(full) || (type != '0' && type != 0 && !is_d)) {
            o_printf(o, "tar: skipped %s\r\n", name);
            if (fseek(f, (long)blocks * 512, SEEK_CUR) != 0) break;
            continue;
        }
        if (is_d) { mkdirs(full); count++; continue; }

        size_t ft = 0, fu = 0;
        if (k85_fs_info(&ft, &fu) && ft > fu && (ft - fu) < (size_t)size + 65536) { o_puts(o, "tar: not enough free space\r\n"); bad = true; break; }
        char parent[300];
        snprintf(parent, sizeof(parent), "%s", full);
        char *sl = strrchr(parent, '/');
        if (sl) { *sl = 0; mkdirs(parent); }
        FILE *out = fopen(full, "wb");
        if (!out) { o_printf(o, "tar: cannot create %s\r\n", name); bad = true; break; }
        uint32_t left = size;
        bool ok = true;
        for (uint32_t b = 0; b < blocks; b++) {
            if (fread(buf, 1, 512, f) != 512) { ok = false; break; }
            size_t w = left < 512 ? left : 512;
            if (fwrite(buf, 1, w, out) != w) { ok = false; break; }
            left -= (uint32_t)w;
        }
        if (fclose(out) != 0) ok = false;
        if (!ok) { unlink(full); o_printf(o, "tar: error writing %s\r\n", name); bad = true; break; }
        count++;
    }
    free(buf);
    fclose(f);
    if (!bad) o_printf(o, "%s: %d entries\r\n", extract ? "extracted" : "listed", count);
}

// scrshot            take a screenshot now (saved to /littlefs/shots/)
// scrshot on|off|status   A+B short-tap gesture
static void cmd_scrshot(int argc, char **argv, Out *o) {
    if (argc >= 1 && !strcmp(argv[0], "on")) { k85_screenshot_set_gesture(true); o_puts(o, "A+B tap screenshot: on\r\n"); return; }
    if (argc >= 1 && !strcmp(argv[0], "off")) { k85_screenshot_set_gesture(false); o_puts(o, "A+B tap screenshot: off\r\n"); return; }
    if (argc >= 1 && !strcmp(argv[0], "status")) { o_printf(o, "A+B tap screenshot: %s\r\n", k85_screenshot_gesture_enabled() ? "on" : "off"); return; }
    char path[64];
    K85ShotResult r = k85_screenshot_take(path, sizeof(path));
    if (r == K85_SHOT_OK) o_printf(o, "saved %s\r\n", strncmp(path, K85_FS_ROOT "/", strlen(K85_FS_ROOT) + 1) == 0 ? path + strlen(K85_FS_ROOT) + 1 : path);
    else o_printf(o, "scrshot: %s\r\n", k85_screenshot_result_text(r));
}

// ---------- dispatcher ----------
struct CmdEntry { const char *name; void (*fn)(int, char **, Out *); };

static const CmdEntry kCmds[] = {
    { "grep", cmd_grep },     { "head", cmd_head },       { "tail", cmd_tail },     { "wc", cmd_wc },
    { "cp", cmd_cp },         { "mv", cmd_mv },           { "rm", cmd_rm },         { "mkdir", cmd_mkdir },
    { "touch", cmd_touch },   { "date", cmd_date },       { "df", cmd_df },         { "du", cmd_du },
    { "hostname", cmd_hostname }, { "ifconfig", cmd_ifconfig }, { "nslookup", cmd_nslookup },
    { "wget", cmd_wget },     { "curl", cmd_curl },       { "watch", cmd_watch },   { "neofetch", cmd_neofetch },
    { "at", cmd_at },         { "tar", cmd_tar },         { "scrprobe", cmd_scrprobe },
    { "scrshot", cmd_scrshot },
};

bool k85_shell_ext_run(const char *cmd, const char *arg, char *out, size_t out_size) {
    for (size_t i = 0; i < sizeof(kCmds) / sizeof(kCmds[0]); i++) {
        if (strcmp(kCmds[i].name, cmd) != 0) continue;
        char buf[220];
        char *argv[MAX_ARGS];
        int argc = tokenize(arg, buf, sizeof(buf), argv, MAX_ARGS);
        Out o;
        o_init(&o, out, out_size);
        kCmds[i].fn(argc, argv, &o);
        return true;
    }
    return false;
}

// ---------- pipes and redirects ----------
#define MAX_STAGES 6

void k85_shell_pipeline(const char *line, char *out, size_t out_size, k85_shell_fn run) {
    if (!has_unquoted(line, '|') && !has_unquoted(line, '>')) { run(line, out, out_size); return; }

    char *work = xalloc(strlen(line) + 1);
    if (!work) { snprintf(out, out_size, "out of memory\r\n"); return; }
    strcpy(work, line);

    char *stage[MAX_STAGES];
    int ns = 0;
    char q = 0;
    stage[ns++] = work;
    for (char *p = work; *p; p++) {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '\'' || *p == '"') { q = *p; continue; }
        if (*p == '|') {
            *p = 0;
            if (ns >= MAX_STAGES) { snprintf(out, out_size, "too many pipes (max 5)\r\n"); free(work); return; }
            stage[ns++] = p + 1;
        }
    }

    for (int i = 0; i < ns; i++) {
        if (!trim(stage[i])[0]) { snprintf(out, out_size, "syntax error: empty command\r\n"); free(work); return; }
    }

    // redirect: only allowed in the last stage
    char *redir_file = nullptr;
    bool append = false;
    for (int i = 0; i < ns; i++) {
        char *s = stage[i];
        q = 0;
        for (char *p = s; *p; p++) {
            if (q) { if (*p == q) q = 0; continue; }
            if (*p == '\'' || *p == '"') { q = *p; continue; }
            if (*p == '>') {
                if (i != ns - 1) { snprintf(out, out_size, "syntax error: redirect only after the last command\r\n"); free(work); return; }
                *p = 0;
                p++;
                if (*p == '>') { append = true; p++; }
                redir_file = trim(p);
                break;
            }
        }
    }
    if (redir_file && (!redir_file[0] || strpbrk(redir_file, " \t><|"))) {
        snprintf(out, out_size, "syntax error: expected one file name after >\r\n");
        free(work);
        return;
    }

    char *bufA = xalloc(out_size);
    char *bufB = ns > 1 ? xalloc(out_size) : nullptr;
    if (!bufA || (ns > 1 && !bufB)) { free(bufA); free(bufB); free(work); snprintf(out, out_size, "out of memory\r\n"); return; }

    const char *in = nullptr;
    char *last = nullptr;
    for (int i = 0; i < ns; i++) {
        bool final_to_out = (i == ns - 1) && !redir_file;
        char *target = final_to_out ? out : ((i % 2 == 0) ? bufA : bufB);
        target[0] = 0;
        tl_stdin = in;
        run(trim(stage[i]), target, out_size);
        tl_stdin = nullptr;
        in = target;
        last = target;
    }

    if (redir_file) {
        out[0] = 0;
        char full[200];
        if (!fs_path(redir_file, full, sizeof(full), false)) {
            snprintf(out, out_size, "redirect: path not allowed: %s\r\n", redir_file);
        } else if (is_dir(full)) {
            snprintf(out, out_size, "redirect: %s is a directory\r\n", redir_file);
        } else {
            FILE *f = fopen(full, append ? "ab" : "wb");
            if (!f) {
                snprintf(out, out_size, "redirect: cannot open %s\r\n", redir_file);
            } else {
                bool ok = true;
                for (const char *p = last; *p && ok; p++) if (*p != '\r') ok = fputc(*p, f) != EOF;
                if (fclose(f) != 0) ok = false;
                if (!ok) snprintf(out, out_size, "redirect: write error (disk full?)\r\n");
            }
        }
    }
    free(bufA);
    free(bufB);
    free(work);
}

#pragma GCC diagnostic pop
