// K85MSG1: offline messenger over ESP-NOW. Chat (everyone / private) + file transfer (stop-and-wait, CRC32).
#include "messenger.h"
#include "config.h"
#include "svc_cfg.h"
#include "fs_guard.h"
#include "wifi.h"
#include "common.h"
#include "input.h"
#include "list_menu.h"
#include "text_input.h"
#include "sound.h"
#include "theme.h"
#include "headless.h"
#include "lang.h"

#include "M5Unified.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define MSG_MAX_FILE  (1024 * 1024)
#define CHUNK         200
#define LOG_N         40
#define NPEER         8
#define RX_DATA       216
#define INBOX         "/littlefs/inbox"

enum { T_HELLO = 1, T_MSG, T_FREQ, T_FACC, T_FDATA, T_FACK, T_FEND, T_FDONE };

struct __attribute__((packed)) Hdr { uint8_t m0, m1, type, flags; uint16_t seq; uint16_t len; };
struct RxItem { uint8_t mac[6]; int8_t rssi; uint16_t len; uint8_t data[RX_DATA]; };

static QueueHandle_t s_q = nullptr;
static uint8_t s_my[6];
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static char s_nick[16];
static bool s_accept = true;
static bool s_dirty = true;
static bool s_busy = false;
static int64_t s_hello_due = 0;
static int s_ch = 1;
static M5Canvas *s_cv = nullptr;

typedef lgfx::LovyanGFX Gfx;
static Gfx *gfx() { return s_cv ? (Gfx *)s_cv : (Gfx *)&M5.Display; }

// ---------------- crc / names ----------------
static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static void sanitize(const char *in, char *out, size_t n) {
    const char *b = strrchr(in, '/');
    if (b) in = b + 1;
    size_t o = 0;
    for (; *in && o + 1 < n && o < 40; in++) {
        unsigned char c = (unsigned char)*in;
        out[o++] = (isalnum(c) || c == '.' || c == '_' || c == '-' || c == ' ') ? (char)c : '_';
    }
    out[o] = 0;
    while (out[0] == '.') memmove(out, out + 1, strlen(out));
    if (strstr(out, "..")) out[0] = 0;
}

// ---------------- peers ----------------
struct Peer { uint8_t mac[6]; char nick[16]; int8_t rssi; int64_t seen; bool used; };
static Peer s_peers[NPEER];

static Peer *peer_find(const uint8_t *mac) {
    for (auto &p : s_peers) if (p.used && !memcmp(p.mac, mac, 6)) return &p;
    return nullptr;
}
static Peer *peer_upsert(const uint8_t *mac, const char *nick, int rssi) {
    Peer *p = peer_find(mac);
    if (!p) {
        p = &s_peers[0];
        for (auto &q : s_peers) {
            if (!q.used) { p = &q; break; }
            if (q.seen < p->seen) p = &q;
        }
        memset(p, 0, sizeof(*p));
        memcpy(p->mac, mac, 6);
        p->used = true;
    }
    if (nick && nick[0]) snprintf(p->nick, sizeof(p->nick), "%s", nick);
    p->rssi = (int8_t)rssi;
    p->seen = esp_timer_get_time();
    return p;
}
static const char *nick_of(const uint8_t *mac, char *buf) {
    Peer *p = peer_find(mac);
    if (p && p->nick[0]) return p->nick;
    snprintf(buf, 16, "%02x%02x", mac[4], mac[5]);
    return buf;
}

// ---------------- message log ----------------
struct Msg { char nick[16]; char text[112]; uint8_t peer[6]; bool all, mine, sys; };
static EXT_RAM_BSS_ATTR Msg s_log[LOG_N];
static int s_log_n = 0;

static void log_add(const char *nick, const char *text, const uint8_t *peer, bool all, bool mine, bool sys) {
    Msg &m = s_log[s_log_n % LOG_N];
    memset(&m, 0, sizeof(m));
    snprintf(m.nick, sizeof(m.nick), "%s", nick);
    snprintf(m.text, sizeof(m.text), "%s", text);
    if (peer) memcpy(m.peer, peer, 6);
    m.all = all; m.mine = mine; m.sys = sys;
    s_log_n++;
    s_dirty = true;
}
static bool visible(const Msg &m, bool all, const uint8_t *peer) {
    if (m.sys) return true;
    if (all) return m.all;
    return !m.all && !memcmp(m.peer, peer, 6);
}

// ---------------- radio ----------------
static void ensure_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    esp_now_add_peer(&p);
}
static bool tx(const uint8_t *mac, uint8_t type, uint8_t flags, uint16_t seq, const void *pl, uint16_t plen) {
    if (plen > 208) return false;
    uint8_t buf[RX_DATA];
    Hdr *h = (Hdr *)buf;
    h->m0 = 'K'; h->m1 = '8'; h->type = type; h->flags = flags; h->seq = seq; h->len = plen;
    if (plen) memcpy(buf + sizeof(Hdr), pl, plen);
    ensure_peer(mac);
    return esp_now_send(mac, buf, sizeof(Hdr) + plen) == ESP_OK;
}
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_q || !info || len < (int)sizeof(Hdr) || len > RX_DATA || data[0] != 'K' || data[1] != '8') return;
    RxItem it;
    memcpy(it.mac, info->src_addr, 6);
    it.rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    it.len = (uint16_t)len;
    memcpy(it.data, data, (size_t)len);
    xQueueSend(s_q, &it, 0);
}

// ---------------- file transfer state ----------------
struct Rx { bool active; FILE *f; uint8_t mac[6]; char path[96]; char part[104]; char name[48]; uint32_t size, got, crc; uint16_t next; int64_t last; };
static Rx s_rx;
static uint8_t s_tx_mac[6];
static int s_tx_acc = -1, s_tx_ack = -1, s_tx_done = -1;

static void rx_abort(const char *why) {
    if (s_rx.f) fclose(s_rx.f);
    s_rx.f = nullptr;
    remove(s_rx.part);
    s_rx.active = false;
    log_add("sys", why, nullptr, false, false, true);
}

static void handle(const RxItem &it) {
    const Hdr *h = (const Hdr *)it.data;
    uint16_t plen = h->len;
    if (sizeof(Hdr) + plen > it.len) return;
    const uint8_t *pl = it.data + sizeof(Hdr);
    char nb[16];
    int64_t now = esp_timer_get_time();
    switch (h->type) {
    case T_HELLO: {
        char nk[16];
        size_t n = plen < 15 ? plen : 15;
        memcpy(nk, pl, n);
        nk[n] = 0;
        bool fresh = (peer_find(it.mac) == nullptr);
        peer_upsert(it.mac, nk, it.rssi);
        if (fresh) s_hello_due = 0;
        s_dirty = true;
        break;
    }
    case T_MSG: {
        char t[112];
        size_t n = plen < 111 ? plen : 111;
        memcpy(t, pl, n);
        t[n] = 0;
        peer_upsert(it.mac, nullptr, it.rssi);
        log_add(nick_of(it.mac, nb), t, it.mac, (h->flags & 1) != 0, false, false);
        k85_play_tone(1800, 50);
        break;
    }
    case T_FREQ: {
        if (plen < 5) break;
        uint32_t size = 0;
        memcpy(&size, pl, 4);
        char raw[48];
        size_t nl = plen - 4;
        if (nl > 47) nl = 47;
        memcpy(raw, pl + 4, nl);
        raw[nl] = 0;
        char clean[48];
        sanitize(raw, clean, sizeof(clean));
        size_t tot = 0, used = 0;
        bool ok = s_accept && !s_rx.active && !s_busy && size > 0 && size <= MSG_MAX_FILE && clean[0]
                  && k85_fs_info(&tot, &used) && tot > used && (size_t)size + 96 * 1024 < tot - used;
        if (ok) {
            mkdir(INBOX, 0755);
            snprintf(s_rx.path, sizeof(s_rx.path), INBOX "/%s", clean);
            ok = !k85_is_blocked_path(s_rx.path);
        }
        if (ok) {
            snprintf(s_rx.part, sizeof(s_rx.part), "%s.part", s_rx.path);
            s_rx.f = fopen(s_rx.part, "wb");
            ok = (s_rx.f != nullptr);
        }
        if (ok) {
            s_rx.active = true;
            memcpy(s_rx.mac, it.mac, 6);
            s_rx.size = size; s_rx.got = 0; s_rx.next = 0; s_rx.crc = 0; s_rx.last = now;
            snprintf(s_rx.name, sizeof(s_rx.name), "%s", clean);
            char m[96];
            snprintf(m, sizeof(m), "receiving %s (%u B)", clean, (unsigned)size);
            log_add("sys", m, nullptr, false, false, true);
        }
        uint8_t r = ok ? 1 : 0;
        tx(it.mac, T_FACC, 0, 0, &r, 1);
        break;
    }
    case T_FACC:
        if (!memcmp(it.mac, s_tx_mac, 6)) s_tx_acc = (plen >= 1 && pl[0]) ? 1 : 0;
        break;
    case T_FDATA:
        if (!s_rx.active || memcmp(it.mac, s_rx.mac, 6)) break;
        s_rx.last = now;
        if (h->seq == s_rx.next) {
            if (s_rx.got + plen > s_rx.size) { rx_abort("file too long"); break; }
            if (fwrite(pl, 1, plen, s_rx.f) != plen) { rx_abort("write error"); break; }
            s_rx.got += plen;
            s_rx.crc = crc32_upd(s_rx.crc, pl, plen);
            s_rx.next++;
            tx(it.mac, T_FACK, 0, h->seq, nullptr, 0);
        } else if ((uint16_t)(h->seq + 1) == s_rx.next) {
            tx(it.mac, T_FACK, 0, h->seq, nullptr, 0);
        }
        break;
    case T_FACK:
        if (!memcmp(it.mac, s_tx_mac, 6)) s_tx_ack = h->seq;
        break;
    case T_FEND: {
        if (!s_rx.active || memcmp(it.mac, s_rx.mac, 6)) break;
        uint32_t crc = 0, total = 0;
        if (plen >= 8) { memcpy(&crc, pl, 4); memcpy(&total, pl + 4, 4); }
        fclose(s_rx.f);
        s_rx.f = nullptr;
        bool ok = plen >= 8 && total == s_rx.got && crc == s_rx.crc && s_rx.got == s_rx.size;
        if (ok) { remove(s_rx.path); ok = (rename(s_rx.part, s_rx.path) == 0); }
        if (!ok) remove(s_rx.part);
        s_rx.active = false;
        tx(it.mac, T_FDONE, ok ? 1 : 0, 0, nullptr, 0);
        char m[96];
        snprintf(m, sizeof(m), ok ? "received %s" : "failed: %s", s_rx.name);
        log_add("sys", m, nullptr, false, false, true);
        k85_play_tone(ok ? 2200 : 400, 80);
        break;
    }
    case T_FDONE:
        if (!memcmp(it.mac, s_tx_mac, 6)) s_tx_done = (h->flags & 1) ? 1 : 0;
        break;
    default:
        break;
    }
}

static void idle(int64_t now) {
    if (s_rx.active && now - s_rx.last > 12000000) rx_abort("receive timed out");
    if (!s_busy && now >= s_hello_due) {
        s_hello_due = now + 3000000;
        tx(BCAST, T_HELLO, 0, 0, s_nick, (uint16_t)(strlen(s_nick) + 1));
    }
}

static void pump(TickType_t wait) {
    RxItem it;
    int n = 0;
    while (xQueueReceive(s_q, &it, n == 0 ? wait : 0) == pdTRUE) {
        handle(it);
        if (++n > 16) break;
    }
    idle(esp_timer_get_time());
}

// ---------------- sending a file ----------------
static bool user_abort(void) {
    k85_input_update();
    return k85_ab_held(500);
}
static void progress(const char *what, uint32_t a, uint32_t b) {
    static int last = -1;
    int pct = b ? (int)((uint64_t)a * 100 / b) : 0;
    if (pct / 3 == last) return;
    last = pct / 3;
    char m[64];
    snprintf(m, sizeof(m), "%s\n%d%%\nhold A+B = cancel", what, pct);
    k85_show_message(m);
}
static void note(const char *m) {
    k85_show_message(m);
    vTaskDelay(pdMS_TO_TICKS(1800));
}

static bool send_file(const uint8_t *mac, const char *path, const char *name) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) { note("Cannot read file"); return false; }
    if (st.st_size > MSG_MAX_FILE) { note("File too big\n(max 1 MB)"); return false; }
    FILE *f = fopen(path, "rb");
    if (!f) { note("Cannot open file"); return false; }
    uint32_t size = (uint32_t)st.st_size;

    s_busy = true;
    memcpy(s_tx_mac, mac, 6);
    uint8_t rq[4 + 48];
    memcpy(rq, &size, 4);
    char nm[44];
    snprintf(nm, sizeof(nm), "%s", name);
    memcpy(rq + 4, nm, strlen(nm) + 1);
    uint16_t rql = (uint16_t)(4 + strlen(nm) + 1);

    bool ok = false;
    bool cancelled = false;
    const char *why = "no answer";
    progress("Waiting...", 0, 1);
    s_tx_acc = -1;
    for (int a = 0; a < 4 && s_tx_acc < 0 && !cancelled; a++) {
        tx(mac, T_FREQ, 0, 0, rq, rql);
        int64_t end = esp_timer_get_time() + 1500000;
        while (esp_timer_get_time() < end && s_tx_acc < 0) {
            pump(1);
            if (user_abort()) { cancelled = true; break; }
        }
    }
    if (s_tx_acc == 0) why = "rejected";
    if (s_tx_acc == 1) {
        static EXT_RAM_BSS_ATTR uint8_t buf[CHUNK];
        uint32_t crc = 0, sent = 0;
        uint16_t seq = 0;
        bool fail = false;
        while (sent < size && !fail && !cancelled) {
            size_t n = fread(buf, 1, CHUNK, f);
            if (n == 0) { fail = true; why = "read error"; break; }
            bool acked = false;
            for (int r = 0; r < 12 && !acked && !cancelled; r++) {
                s_tx_ack = -1;
                tx(mac, T_FDATA, 0, seq, buf, (uint16_t)n);
                int64_t end = esp_timer_get_time() + 150000;
                while (esp_timer_get_time() < end) {
                    pump(1);
                    if (s_tx_ack == (int)seq) { acked = true; break; }
                    if (user_abort()) { cancelled = true; break; }
                }
            }
            if (!acked) { fail = true; why = cancelled ? "cancelled" : "lost the link"; break; }
            crc = crc32_upd(crc, buf, n);
            sent += (uint32_t)n;
            seq++;
            progress("Sending", sent, size);
        }
        if (!fail && !cancelled) {
            uint32_t fe[2] = { crc, sent };
            s_tx_done = -1;
            for (int a = 0; a < 4 && s_tx_done < 0 && !cancelled; a++) {
                tx(mac, T_FEND, 0, 0, fe, 8);
                int64_t end = esp_timer_get_time() + 1500000;
                while (esp_timer_get_time() < end && s_tx_done < 0) {
                    pump(1);
                    if (user_abort()) { cancelled = true; break; }
                }
            }
            ok = (s_tx_done == 1);
            if (!ok) why = "check failed";
        }
    }
    fclose(f);
    s_busy = false;
    if (cancelled) { k85_wait_ab_release(); why = "cancelled"; }
    char m[64];
    snprintf(m, sizeof(m), ok ? "File sent" : "Not sent:\n%s", why);
    note(m);
    s_dirty = true;
    return ok;
}

// ---------------- picking a file ----------------
static bool pick_file(char *path_out, size_t pn, char *name_out, size_t nn) {
    static EXT_RAM_BSS_ATTR char names[40][44];
    static EXT_RAM_BSS_ATTR char labels[42][46];
    bool isdir[40];
    const char *items[42];
    char dir[110] = "/littlefs";
    while (true) {
        int cnt = 0;
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != nullptr && cnt < 40) {
                const char *nm = e->d_name;
                if (!nm[0] || nm[0] == '.' || strlen(nm) > 40) continue;
                char fp[170];
                snprintf(fp, sizeof(fp), "%s/%s", dir, nm);
                struct stat st;
                if (stat(fp, &st) != 0 || k85_is_blocked_path(fp)) continue;
                snprintf(names[cnt], sizeof(names[0]), "%s", nm);
                isdir[cnt] = S_ISDIR(st.st_mode);
                cnt++;
            }
            closedir(d);
        }
        int n = 0;
        int up = -1;
        if (strcmp(dir, "/littlefs") != 0) { snprintf(labels[n], sizeof(labels[0]), ".. up"); items[n] = labels[n]; up = n; n++; }
        int base = n;
        for (int i = 0; i < cnt; i++) {
            snprintf(labels[n], sizeof(labels[0]), isdir[i] ? "%.40s/" : "%.40s", names[i]);
            items[n] = labels[n];
            n++;
        }
        items[n] = "Cancel";
        int sel = k85_run_list_menu("SEND FILE", items, n + 1, nullptr);
        if (sel < 0 || sel >= n) return false;
        if (sel == up) { char *s = strrchr(dir, '/'); if (s && s != dir) *s = 0; continue; }
        int i = sel - base;
        char full[170];
        snprintf(full, sizeof(full), "%s/%s", dir, names[i]);
        if (isdir[i]) { snprintf(dir, sizeof(dir), "%s", full); continue; }
        snprintf(path_out, pn, "%s", full);
        snprintf(name_out, nn, "%s", names[i]);
        return true;
    }
}

// ---------------- drawing ----------------
static int wrap_lines(const char *pre, const char *text, char lines[][44], int max, int n, int cols) {
    char all[160];
    snprintf(all, sizeof(all), "%s: %s", pre, text);
    size_t len = strlen(all);
    for (size_t o = 0; o < len || o == 0; o += (size_t)cols) {
        if (n >= max) { memmove(lines[0], lines[1], (size_t)(max - 1) * 44); n = max - 1; }
        snprintf(lines[n], 44, "%.*s", cols, all + o);
        n++;
        if (len == 0) break;
    }
    return n;
}

static void draw_chat(bool all, const uint8_t *peer, const char *title, int off) {
    Gfx *g = gfx();
    uint32_t bg = k85_get_bg(), fg = k85_get_fg(), accent = k85_get_accent();
    int W = M5.Display.width(), H = M5.Display.height();
    int cols = W / 6 - 1;
    if (cols > 43) cols = 43;
    if (cols < 8) cols = 8;
    static EXT_RAM_BSS_ATTR char lines[100][44];
    static EXT_RAM_BSS_ATTR uint8_t kind[100];
    int n = 0;
    int start = s_log_n > LOG_N ? s_log_n - LOG_N : 0;
    for (int i = start; i < s_log_n; i++) {
        const Msg &m = s_log[i % LOG_N];
        if (!visible(m, all, peer)) continue;
        int before = n;
        n = wrap_lines(m.sys ? "*" : (m.mine ? "me" : m.nick), m.text, lines, 100, n, cols);
        for (int k = before; k < n; k++) kind[k] = m.sys ? 2 : (m.mine ? 1 : 0);
        if (n == 100) for (int k = 0; k < 100; k++) if (k >= before) kind[k] = m.sys ? 2 : (m.mine ? 1 : 0);
    }
    int rows = (H - 22) / 9;
    if (rows < 1) rows = 1;
    int maxoff = n > rows ? n - rows : 0;
    if (off > maxoff) off = maxoff;
    g->fillRect(0, 0, W, H, bg);
    g->setTextSize(1);
    g->setTextColor(accent, bg);
    g->setCursor(2, 1);
    g->printf("%.14s  ch%d", title, s_ch);
    int first = n - rows - off;
    if (first < 0) first = 0;
    for (int r = 0; r < rows && first + r < n; r++) {
        int k = first + r;
        g->setTextColor(kind[k] == 1 ? accent : (kind[k] == 2 ? 0x888888 : fg), bg);
        g->setCursor(2, 12 + r * 9);
        g->print(lines[k]);
    }
    g->setTextColor(0x888888, bg);
    g->setCursor(2, H - 9);
    g->print(W >= 200 ? "A=write B=menu hold A+B=back" : "A=write B=menu");
    if (s_cv) s_cv->pushSprite(0, 0);
}

static void chat(bool all, const uint8_t *peer_mac, const char *title) {
    uint8_t peer[6];
    memcpy(peer, peer_mac, 6);
    int off = 0;
    int64_t last = 0;
    s_dirty = true;
    while (true) {
        k85_input_update();
        pump(1);
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed()) {
            static EXT_RAM_BSS_ATTR char txt[112];
            txt[0] = 0;
            char pr[24];
            snprintf(pr, sizeof(pr), "To %.12s:", title);
            if (k85_text_input(pr, "", txt, sizeof(txt)) && txt[0]) {
                tx(all ? BCAST : peer, T_MSG, all ? 1 : 0, 0, txt, (uint16_t)(strlen(txt) + 1));
                log_add("me", txt, peer, all, true, false);
                off = 0;
            }
            s_dirty = true;
        }
        if (k85_btn_b_pressed()) {
            const char *items[] = { all ? "(file: private chat only)" : "Send file", "Scroll older", "Newest", "Back" };
            int s = k85_run_list_menu("CHAT", items, 4, nullptr);
            if (s == 0 && !all) {
                char path[170], name[44];
                if (pick_file(path, sizeof(path), name, sizeof(name))) send_file(peer, path, name);
            } else if (s == 1) off += 5;
            else if (s == 2) off = 0;
            else if (s == 3 || s < 0) return;
            s_dirty = true;
        }
        int64_t now = esp_timer_get_time();
        if (s_dirty || now - last > 500000) { s_dirty = false; last = now; draw_chat(all, peer, title, off); }
    }
}

static void clean_nick(char *s) {
    size_t o = 0;
    for (size_t i = 0; s[i] && o < 12; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_') s[o++] = (char)c;
    }
    s[o] = 0;
}

static void show_inbox(void) {
    static EXT_RAM_BSS_ATTR char labels[22][44];
    const char *items[22];
    int n = 0;
    DIR *d = opendir(INBOX);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr && n < 20) {
            if (e->d_name[0] == '.' || strlen(e->d_name) > 38) continue;
            char fp[140];
            snprintf(fp, sizeof(fp), INBOX "/%s", e->d_name);
            struct stat st;
            if (stat(fp, &st) != 0) continue;
            snprintf(labels[n], sizeof(labels[0]), "%.30s %uK", e->d_name, (unsigned)(st.st_size / 1024));
            items[n] = labels[n];
            n++;
        }
        closedir(d);
    }
    if (n == 0) { note("Inbox is empty"); return; }
    items[n] = "Back";
    k85_run_list_menu("INBOX", items, n + 1, nullptr);
}

static void apply_channel(void) {
    if (k85_headless_active()) { esp_wifi_disconnect(); wifi_config_t c = {}; if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) { c.ap.channel = (uint8_t)s_ch; esp_wifi_set_config(WIFI_IF_AP, &c); } }
    esp_wifi_set_channel((uint8_t)s_ch, WIFI_SECOND_CHAN_NONE);
}

void k85_run_messenger(void) {
    k85_wifi_init();
    bool hl = k85_headless_active();
    char v[8];
    if (k85_svc_get_str("msg_ch", v, sizeof(v))) { int c = atoi(v); if (c >= 1 && c <= 13) s_ch = c; }
    s_accept = k85_svc_get_bool("msg_rx", true);
    esp_wifi_get_mac(WIFI_IF_STA, s_my);
    uint8_t orig_ch = 1; { wifi_second_chan_t os; esp_wifi_get_channel(&orig_ch, &os); } if (hl) s_ch = orig_ch;
    char nk[16];
    if (k85_svc_get_str("msg_nick", nk, sizeof(nk)) && nk[0]) snprintf(s_nick, sizeof(s_nick), "%s", nk);
    else snprintf(s_nick, sizeof(s_nick), "k85-%02X%02X", s_my[4], s_my[5]);

    if (!hl) {
        esp_wifi_disconnect();           // leave the router: all sticks must sit on one channel
        vTaskDelay(pdMS_TO_TICKS(300));
        apply_channel();
    }
    esp_wifi_set_ps(WIFI_PS_NONE);       // modem sleep would make the stick miss packets

    s_q = xQueueCreate(10, sizeof(RxItem));
    if (!s_q || esp_now_init() != ESP_OK) {
        if (s_q) { vQueueDelete(s_q); s_q = nullptr; }
        note("ESP-NOW failed\n(no WiFi driver?)");
        return;
    }
    esp_now_register_recv_cb(on_recv);
    ensure_peer(BCAST);
    memset(s_peers, 0, sizeof(s_peers));
    memset(&s_rx, 0, sizeof(s_rx));
    s_hello_due = 0;
    s_busy = false;

    s_cv = new M5Canvas(&M5.Display);
    s_cv->setPsram(true);
    s_cv->setFont(k85_ru_font());
    if (!s_cv->createSprite(M5.Display.width(), M5.Display.height())) { delete s_cv; s_cv = nullptr; }

    static EXT_RAM_BSS_ATTR char labels[NPEER + 6][40];
    const char *items[NPEER + 6];
    int pidx[NPEER];
    int sel = 0;
    int64_t last = 0;
    s_dirty = true;

    while (true) {
        k85_input_update();
        pump(1);
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }

        int np = 0;
        for (int i = 0; i < NPEER; i++) if (s_peers[i].used) pidx[np++] = i;
        int n = 0;
        snprintf(labels[n], sizeof(labels[0]), "Everyone"); items[n] = labels[n]; n++;
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < np; i++) {
            const Peer &p = s_peers[pidx[i]];
            bool away = now - p.seen > 20000000;
            snprintf(labels[n], sizeof(labels[0]), away ? "%.12s (away)" : "%.12s %ddBm", p.nick[0] ? p.nick : "?", (int)p.rssi);
            items[n] = labels[n]; n++;
        }
        int iNick = n;   snprintf(labels[n], sizeof(labels[0]), "Nick: %.12s", s_nick); items[n] = labels[n]; n++;
        int iCh = n;
        uint8_t pc = 0; wifi_second_chan_t sc;
        esp_wifi_get_channel(&pc, &sc);
        if (hl) snprintf(labels[n], sizeof(labels[0]), "Channel: %d", (int)pc);
        else snprintf(labels[n], sizeof(labels[0]), "Channel: %d", s_ch);
        items[n] = labels[n]; n++;
        int iRx = n;     snprintf(labels[n], sizeof(labels[0]), "Get files: %s", s_accept ? "ON" : "OFF"); items[n] = labels[n]; n++;
        int iInbox = n;  snprintf(labels[n], sizeof(labels[0]), "Inbox files"); items[n] = labels[n]; n++;
        int iExit = n;   snprintf(labels[n], sizeof(labels[0]), "Exit"); items[n] = labels[n]; n++;
        if (sel >= n) sel = n - 1;

        { const int k85_bk = k85_a_hold_back(2); if (k85_bk > 0) { sel = ((sel - k85_bk) % n + n) % n; s_dirty = true; } } /*B2:msg*/ if (k85_btn_a_pressed()) { sel = (sel + 1) % n; s_dirty = true; }
        if (k85_btn_b_pressed()) {
            s_dirty = true;
            if (sel == 0) chat(true, BCAST, "Everyone");
            else if (sel < iNick) {
                Peer &p = s_peers[pidx[sel - 1]];
                chat(false, p.mac, p.nick[0] ? p.nick : "peer");
            } else if (sel == iNick) {
                char t[16];
                snprintf(t, sizeof(t), "%s", s_nick);
                if (k85_text_input("Nickname:", t, t, sizeof(t))) {
                    clean_nick(t);
                    if (t[0]) { snprintf(s_nick, sizeof(s_nick), "%s", t); k85_svc_set_str("msg_nick", s_nick); s_hello_due = 0; }
                }
            } else if (sel == iCh) { if (hl) { char t[4]; snprintf(t, sizeof(t), "%d", (int)pc); if (k85_text_input("Channel 1-13:", t, t, sizeof(t))) { int nc = atoi(t); if (nc >= 1 && nc <= 13) { s_ch = nc; apply_channel(); char cs[8]; snprintf(cs, sizeof(cs), "%d", s_ch); k85_svc_set_str("msg_ch", cs); } } } 
                if (!hl) {
                    s_ch = s_ch % 13 + 1;
                    apply_channel();
                    char c[8];
                    snprintf(c, sizeof(c), "%d", s_ch);
                    k85_svc_set_str("msg_ch", c);
                }
            } else if (sel == iRx) {
                s_accept = !s_accept;
                k85_svc_set_bool("msg_rx", s_accept);
            } else if (sel == iInbox) {
                show_inbox();
            } else if (sel == iExit) {
                break;
            }
        }

        if (s_dirty || now - last > 500000) {
            s_dirty = false;
            last = now;
            Gfx *g = gfx();
            uint32_t bg = k85_get_bg(), fg = k85_get_fg(), accent = k85_get_accent();
            int W = M5.Display.width(), H = M5.Display.height();
            g->fillRect(0, 0, W, H, bg);
            g->setTextSize(1);
            g->setTextColor(accent, bg);
            g->setCursor(2, 1);
            g->printf("MESSENGER  ch%d  %d near", (int)pc, np);
            int rows = (H - 22) / 11;
            if (rows < 1) rows = 1;
            int top = sel >= rows ? sel - rows + 1 : 0;
            for (int r = 0; r < rows && top + r < n; r++) {
                int k = top + r;
                bool on = (k == sel);
                if (on) g->fillRect(0, 12 + r * 11 - 1, W, 11, accent);
                g->setTextColor(on ? 0x000000 : fg, on ? accent : bg);
                g->setCursor(4, 12 + r * 11);
                g->print(items[k]);
            }
            g->setTextColor(0x888888, bg);
            g->setCursor(2, H - 9);
            g->print(W >= 200 ? "A=next B=open hold A+B=exit" : "A=next B=open");
            if (s_cv) s_cv->pushSprite(0, 0);
        }
    }

    if (s_rx.active) rx_abort("closed");
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    QueueHandle_t q = s_q;
    s_q = nullptr;
    vQueueDelete(q);
    if (s_cv) { s_cv->deleteSprite(); delete s_cv; s_cv = nullptr; }
    if (!hl) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (g_config.wifi_saved && !g_config.wifi_disabled) esp_wifi_connect();   // back to the router
    }
    if (hl) { if ((int)orig_ch != s_ch) { s_ch = orig_ch; apply_channel(); } if (g_config.wifi_saved && !g_config.wifi_disabled) esp_wifi_connect(); }
    M5.Display.fillScreen(k85_get_bg());
}