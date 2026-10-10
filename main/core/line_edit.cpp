#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "line_edit.h"
#include "fs_guard.h"

#include "esp_heap_caps.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#ifndef K85_FS_ROOT
#define K85_FS_ROOT "/littlefs"
#endif

// ---------- small output buffer ----------
struct Obuf { char *b; size_t cap, len; };

static void ob_init(Obuf *o, char *b, size_t cap) { o->b = b; o->cap = cap; o->len = 0; if (cap) b[0] = 0; }
static void ob_putn(Obuf *o, const char *s, size_t n) {
    if (!o->cap) return;
    if (o->len + n + 1 > o->cap) n = (o->cap > o->len + 1) ? o->cap - o->len - 1 : 0;
    memcpy(o->b + o->len, s, n);
    o->len += n;
    o->b[o->len] = 0;
}
static void ob_puts(Obuf *o, const char *s) { ob_putn(o, s, strlen(s)); }

// ---------- history (shared by all editors, lives in PSRAM) ----------
static char (*s_hist)[K85_LE_MAX] = nullptr;
static int s_hist_count = 0;

static bool hist_alloc(void) {
    if (s_hist) return true;
    void *p = heap_caps_calloc(K85_LE_HIST, K85_LE_MAX, MALLOC_CAP_SPIRAM);
    if (!p) p = calloc(K85_LE_HIST, K85_LE_MAX);
    s_hist = (char (*)[K85_LE_MAX])p;
    return s_hist != nullptr;
}

void k85_le_history_add(const char *line) {
    if (!line || !line[0]) return;
    if (!strncmp(line, "ftppass", 7)) return;                // never keep passwords
    if (!hist_alloc()) return;
    if (s_hist_count > 0 && !strcmp(s_hist[0], line)) return;
    int n = s_hist_count < K85_LE_HIST ? s_hist_count : K85_LE_HIST - 1;
    memmove(s_hist[1], s_hist[0], (size_t)n * K85_LE_MAX);
    snprintf(s_hist[0], K85_LE_MAX, "%s", line);
    if (s_hist_count < K85_LE_HIST) s_hist_count++;
}

int k85_le_history_count(void) { return s_hist_count; }

const char *k85_le_history_get(int idx) {
    if (!s_hist || idx < 0 || idx >= s_hist_count) return nullptr;
    return s_hist[idx];
}

// ---------- completion ----------
static const char *const kCommands[] = {
    "alias", "at", "battery", "brightness", "cat", "cp", "cron", "curl", "date", "df", "du", "echo", "exit",
    "free", "ftp", "ftppass", "grep", "head", "help", "hostname", "htop", "ifconfig", "imu", "info", "ls",
    "mkdir", "mv", "neofetch", "nslookup", "ps", "reboot", "rm", "scrprobe", "scrshot", "set", "sh", "sleep",
    "tail", "tar", "touch", "unalias", "uptime", "volume", "watch", "wc", "wget", "wifi", "wifi-off", "wifi-on",
};

#define MAX_CAND 32
struct Cand { char text[56]; };   // full replacement text for the word (dir part + name [+ '/'])

static int collect_commands(const char *prefix, Cand *c) {
    int n = 0;
    size_t pl = strlen(prefix);
    for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]) && n < MAX_CAND; i++) {
        if (!strncmp(kCommands[i], prefix, pl)) snprintf(c[n++].text, sizeof(c[0].text), "%s", kCommands[i]);
    }
    return n;
}

static int collect_files(const char *word, Cand *c, size_t *dirlen_out) {
    const char *w = word;
    while (*w == '/') w++;
    const char *slash = strrchr(w, '/');
    char dirpart[96] = "";
    const char *prefix = w;
    if (slash) {
        size_t dl = (size_t)(slash - w) + 1;
        if (dl >= sizeof(dirpart)) return 0;
        memcpy(dirpart, w, dl);
        dirpart[dl] = 0;
        prefix = slash + 1;
    }
    if (strstr(dirpart, "..")) return 0;
    *dirlen_out = strlen(dirpart);

    char dir[200];
    if (dirpart[0]) {
        snprintf(dir, sizeof(dir), "%s/%s", K85_FS_ROOT, dirpart);
        size_t l = strlen(dir);
        while (l > 1 && dir[l - 1] == '/') dir[--l] = 0;
    } else {
        snprintf(dir, sizeof(dir), "%s", K85_FS_ROOT);
    }
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    size_t pl = strlen(prefix);
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && n < MAX_CAND) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (nm[0] == '.' && prefix[0] != '.') continue;
        if (strchr(nm, ' ')) continue;                       // names with blanks need quoting: skip
        if (strncmp(nm, prefix, pl) != 0) continue;
        char full[300];
        snprintf(full, sizeof(full), "%s/%s", dir, nm);
        if (k85_is_blocked_path(full)) continue;
        struct stat st;
        bool is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        if (strlen(dirpart) + strlen(nm) + 2 >= sizeof(c[0].text)) continue;
        snprintf(c[n++].text, sizeof(c[0].text), "%s%s%s", dirpart, nm, is_dir ? "/" : "");
    }
    closedir(d);
    return n;
}

static int cmp_cand(const void *a, const void *b) { return strcmp(((const Cand *)a)->text, ((const Cand *)b)->text); }

static void redraw(struct K85LineEditor *e, Obuf *o) {
    ob_puts(o, "\r\x1b[K");
    ob_puts(o, e->prompt);
    ob_putn(o, e->buf, (size_t)e->len);
    int back = e->len - e->cur;
    if (back > 0) {
        char t[16];
        snprintf(t, sizeof(t), "\x1b[%dD", back);
        ob_puts(o, t);
    }
}

// replaces buf[ws..cur) with text
static void replace_word(struct K85LineEditor *e, int ws, const char *text, bool add_space) {
    char tail[K85_LE_MAX];
    snprintf(tail, sizeof(tail), "%s", e->buf + e->cur);
    char head[K85_LE_MAX];
    snprintf(head, sizeof(head), "%.*s", ws, e->buf);
    char out[K85_LE_MAX];
    snprintf(out, sizeof(out), "%s%s%s", head, text, add_space ? " " : "");
    int newcur = (int)strlen(out);
    if (newcur + (int)strlen(tail) >= K85_LE_MAX) return;     // would not fit
    snprintf(e->buf, K85_LE_MAX, "%s%s", out, tail);
    e->len = (int)strlen(e->buf);
    e->cur = newcur;
}

static void do_tab(struct K85LineEditor *e, Obuf *o, bool second) {
    int ws = e->cur;
    while (ws > 0 && e->buf[ws - 1] != ' ' && e->buf[ws - 1] != '|' && e->buf[ws - 1] != '>') ws--;
    char word[96];
    int wl = e->cur - ws;
    if (wl >= (int)sizeof(word)) { ob_puts(o, "\a"); return; }
    memcpy(word, e->buf + ws, (size_t)wl);
    word[wl] = 0;

    // command position: first word of the line or right after a '|'
    int p = ws;
    while (p > 0 && e->buf[p - 1] == ' ') p--;
    bool is_cmd = (p == 0) || (e->buf[p - 1] == '|');
    if (ws > 0 && e->buf[ws - 1] != ' ' && e->buf[ws - 1] != '|') is_cmd = false;

    Cand *cand = (Cand *)calloc(MAX_CAND, sizeof(Cand));
    if (!cand) return;
    size_t dirlen = 0;
    int n = is_cmd ? collect_commands(word, cand) : collect_files(word, cand, &dirlen);
    if (n > 1) qsort(cand, (size_t)n, sizeof(Cand), cmp_cand);

    if (n == 0) {
        ob_puts(o, "\a");
    } else if (n == 1) {
        size_t tl = strlen(cand[0].text);
        bool is_dir = tl && cand[0].text[tl - 1] == '/';
        replace_word(e, ws, cand[0].text, !is_dir);             // names are root-relative: a leading '/' is dropped
        redraw(e, o);
    } else {
        // longest common prefix
        size_t cp = strlen(cand[0].text);
        for (int i = 1; i < n; i++) {
            size_t k = 0;
            while (k < cp && cand[i].text[k] == cand[0].text[k]) k++;
            cp = k;
        }
        const char *w = word;
        while (*w == '/' && !is_cmd) w++;
        if (cp > strlen(w)) {
            char pre[56];
            snprintf(pre, sizeof(pre), "%.*s", (int)cp, cand[0].text);
            replace_word(e, ws, pre, false);
            redraw(e, o);
            e->tab_pending = true;                              // next Tab lists the remaining candidates
        } else if (second) {
            ob_puts(o, "\r\n");
            size_t col = 0;
            for (int i = 0; i < n; i++) {
                const char *shown = cand[i].text + dirlen;
                size_t sl = strlen(shown);
                if (col + sl + 2 > 72 && col > 0) { ob_puts(o, "\r\n"); col = 0; }
                ob_puts(o, shown);
                ob_puts(o, "  ");
                col += sl + 2;
            }
            ob_puts(o, "\r\n");
            redraw(e, o);
        } else {
            ob_puts(o, "\a");
            e->tab_pending = true;
        }
    }
    free(cand);
}

// ---------- editing ----------
void k85_le_init(struct K85LineEditor *e, const char *prompt) {
    memset(e, 0, sizeof(*e));
    e->hist_pos = -1;
    e->prompt = prompt ? prompt : "> ";
}

void k85_le_reset(struct K85LineEditor *e) {
    e->len = e->cur = 0;
    e->buf[0] = 0;
    e->hist_pos = -1;
    e->esc_state = 0;
    e->tab_pending = false;
}

static void load_line(struct K85LineEditor *e, const char *s) {
    snprintf(e->buf, K85_LE_MAX, "%s", s);
    e->len = (int)strlen(e->buf);
    e->cur = e->len;
}

static void history_up(struct K85LineEditor *e, Obuf *o) {
    if (e->hist_pos + 1 >= s_hist_count) { ob_puts(o, "\a"); return; }
    if (e->hist_pos < 0) { memcpy(e->saved, e->buf, sizeof(e->saved)); }
    e->hist_pos++;
    load_line(e, s_hist[e->hist_pos]);
    redraw(e, o);
}

static void history_down(struct K85LineEditor *e, Obuf *o) {
    if (e->hist_pos < 0) { ob_puts(o, "\a"); return; }
    e->hist_pos--;
    if (e->hist_pos < 0) load_line(e, e->saved);
    else load_line(e, s_hist[e->hist_pos]);
    redraw(e, o);
}

static void erase_range(struct K85LineEditor *e, int from, int to) {     // removes buf[from..to)
    memmove(e->buf + from, e->buf + to, (size_t)(e->len - to + 1));
    e->len -= to - from;
    e->cur = from;
}

static void csi_key(struct K85LineEditor *e, char final, Obuf *o) {
    switch (final) {
        case 'A': history_up(e, o); break;
        case 'B': history_down(e, o); break;
        case 'C': if (e->cur < e->len) { e->cur++; ob_puts(o, "\x1b[C"); } break;
        case 'D': if (e->cur > 0) { e->cur--; ob_puts(o, "\x1b[D"); } break;
        case 'H': e->cur = 0; redraw(e, o); break;
        case 'F': e->cur = e->len; redraw(e, o); break;
        case '~':
            if (e->csi_arg == 3) {                                  // Delete
                if (e->cur < e->len) { e->hist_pos = -1; erase_range(e, e->cur, e->cur + 1); redraw(e, o); }
            } else if (e->csi_arg == 1 || e->csi_arg == 7) { e->cur = 0; redraw(e, o); }
            else if (e->csi_arg == 4 || e->csi_arg == 8) { e->cur = e->len; redraw(e, o); }
            break;
        default: break;
    }
}

enum K85LeAction k85_le_feed(struct K85LineEditor *e, uint8_t c, char *out, size_t out_size) {
    Obuf o;
    ob_init(&o, out, out_size);
    hist_alloc();

    bool was_cr = e->last_was_cr;
    bool was_tab = e->tab_pending;
    e->last_was_cr = false;
    e->tab_pending = false;

    if (e->esc_state == 1) {
        if (c == '[') { e->esc_state = 2; e->csi_arg = 0; }
        else if (c == 'O') e->esc_state = 3;
        else e->esc_state = 0;
        return K85_LE_NONE;
    }
    if (e->esc_state == 2) {
        if (c >= '0' && c <= '9') { e->csi_arg = e->csi_arg * 10 + (c - '0'); return K85_LE_NONE; }
        if (c == ';') { e->csi_arg = 0; return K85_LE_NONE; }
        e->esc_state = 0;
        csi_key(e, (char)c, &o);
        return K85_LE_NONE;
    }
    if (e->esc_state == 3) {
        e->esc_state = 0;
        if (c == 'H') { e->cur = 0; redraw(e, &o); }
        else if (c == 'F') { e->cur = e->len; redraw(e, &o); }
        return K85_LE_NONE;
    }

    switch (c) {
        case 27: e->esc_state = 1; return K85_LE_NONE;

        case '\r':
        case '\n':
            if (c == '\n' && was_cr) return K85_LE_NONE;            // CRLF from some clients
            e->last_was_cr = (c == '\r');
            e->buf[e->len] = 0;
            e->hist_pos = -1;
            ob_puts(&o, "\r\n");
            return K85_LE_LINE;

        case 127:
        case 8:
            if (e->cur > 0) {
                e->hist_pos = -1;
                bool at_end = (e->cur == e->len);
                erase_range(e, e->cur - 1, e->cur);
                if (at_end) ob_puts(&o, "\b \b"); else redraw(e, &o);
            }
            return K85_LE_NONE;

        case 9:
            do_tab(e, &o, was_tab);
            return K85_LE_NONE;

        case 1: e->cur = 0; redraw(e, &o); return K85_LE_NONE;           // Ctrl-A
        case 5: e->cur = e->len; redraw(e, &o); return K85_LE_NONE;      // Ctrl-E
        case 21:                                                         // Ctrl-U
            if (e->cur > 0) { e->hist_pos = -1; erase_range(e, 0, e->cur); redraw(e, &o); }
            return K85_LE_NONE;
        case 11:                                                         // Ctrl-K
            if (e->cur < e->len) { e->hist_pos = -1; e->len = e->cur; e->buf[e->len] = 0; redraw(e, &o); }
            return K85_LE_NONE;
        case 23: {                                                       // Ctrl-W
            int i = e->cur;
            while (i > 0 && e->buf[i - 1] == ' ') i--;
            while (i > 0 && e->buf[i - 1] != ' ') i--;
            if (i < e->cur) { e->hist_pos = -1; erase_range(e, i, e->cur); redraw(e, &o); }
            return K85_LE_NONE;
        }
        case 12: ob_puts(&o, "\x1b[2J\x1b[H"); redraw(e, &o); return K85_LE_NONE;   // Ctrl-L
        case 3:                                                          // Ctrl-C
            ob_puts(&o, "^C\r\n");
            k85_le_reset(e);
            ob_puts(&o, e->prompt);
            return K85_LE_CANCEL;
        case 4:                                                          // Ctrl-D
            if (e->len == 0) { ob_puts(&o, "\r\n"); return K85_LE_EOF; }
            if (e->cur < e->len) { e->hist_pos = -1; erase_range(e, e->cur, e->cur + 1); redraw(e, &o); }
            return K85_LE_NONE;
        default: break;
    }

    if (c >= 32 && c != 127 && e->len < K85_LE_MAX - 1) {
        e->hist_pos = -1;
        memmove(e->buf + e->cur + 1, e->buf + e->cur, (size_t)(e->len - e->cur + 1));
        e->buf[e->cur] = (char)c;
        e->len++;
        e->cur++;
        if (e->cur == e->len) ob_putn(&o, (const char *)&c, 1);      // plain echo at the end of the line
        else redraw(e, &o);
    }
    return K85_LE_NONE;
}

#pragma GCC diagnostic pop
