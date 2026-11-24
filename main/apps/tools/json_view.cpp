// K85JV1: JSON viewer for ANY JSON. A lenient parser (comments, trailing commas, single quotes, bare keys, NaN, undefined,
// BOM, JSON Lines) builds a flat node tree in PSRAM. A = next line, hold A = back, B = open / close a container
// (or show the whole value), hold B = menu, A+B = exit.
#include "json_view.h"
#include "json_interpreter.h"
#include "fs_guard.h"
#include "clipboard.h"
#include "common.h"
#include "list_menu.h"
#include "text_input.h"
#include "theme.h"
#include "input.h"
#include "power.h"
#include "lang.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include <sys/stat.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

#define JV_NONE 0xFFFFFFFFu
#define JV_MAX_NODES 20000
#define JV_MAX_FILE (768 * 1024)
#define JV_MAX_DEPTH 60

enum { JT_OBJ = 0, JT_ARR, JT_STR, JT_NUM, JT_BOOL, JT_NULL };

struct JNode {
    uint8_t type, exp;
    uint16_t depth;
    int32_t parent, first, last, next;
    uint32_t idx, nchild;
    uint32_t key, val, vlen;
};

struct JDoc {
    JNode *n;
    int cnt;
    char *pool;
    size_t plen, pcap;
    int32_t *vis;
    int root;
    bool trunc;
};

static void *jv_alloc(size_t n) {
    if (n == 0) n = 1;
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = calloc(1, n);
    return p;
}

static void jv_close(JDoc &d) {
    free(d.n);
    free(d.pool);
    free(d.vis);
    memset(&d, 0, sizeof(d));
}

static bool jv_open(JDoc &d, size_t src_n) {
    memset(&d, 0, sizeof(d));
    d.n = (JNode *)jv_alloc(sizeof(JNode) * JV_MAX_NODES);
    d.pcap = src_n * 2 + 64;
    if (d.pcap < 4096) d.pcap = 4096;
    d.pool = (char *)jv_alloc(d.pcap);
    d.vis = (int32_t *)jv_alloc(sizeof(int32_t) * (JV_MAX_NODES + 1));
    if (!d.n || !d.pool || !d.vis) { jv_close(d); return false; }
    d.root = -1;
    return true;
}

// ---------------- parser ----------------
static bool jv_pc(JDoc &d, char c) {
    if (d.plen >= d.pcap) { d.trunc = true; return false; }
    d.pool[d.plen++] = c;
    return true;
}

static bool jv_pcp(JDoc &d, uint32_t cp) {
    if (cp < 0x80) return jv_pc(d, (char)cp);
    if (cp < 0x800) return jv_pc(d, (char)(0xC0 | (cp >> 6))) && jv_pc(d, (char)(0x80 | (cp & 0x3F)));
    if (cp < 0x10000) {
        return jv_pc(d, (char)(0xE0 | (cp >> 12))) && jv_pc(d, (char)(0x80 | ((cp >> 6) & 0x3F))) && jv_pc(d, (char)(0x80 | (cp & 0x3F)));
    }
    return jv_pc(d, (char)(0xF0 | (cp >> 18))) && jv_pc(d, (char)(0x80 | ((cp >> 12) & 0x3F))) &&
           jv_pc(d, (char)(0x80 | ((cp >> 6) & 0x3F))) && jv_pc(d, (char)(0x80 | (cp & 0x3F)));
}

static uint32_t jv_hx(char c) {
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
    return (uint32_t)(c - 'A' + 10);
}

static void jv_ws(const char *s, size_t n, size_t &i) {
    for (;;) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') { while (i < n && s[i] != '\n') i++; continue; }
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        if (i < n && s[i] == '#') { while (i < n && s[i] != '\n') i++; continue; }
        break;
    }
}

static bool jv_str(JDoc &d, const char *s, size_t n, size_t &i, uint32_t &off) {
    const char q = s[i++];
    off = (uint32_t)d.plen;
    while (i < n && s[i] != q) {
        char c = s[i++];
        if (c == '\\' && i < n) {
            const char e = s[i++];
            if (e == 'u') {
                uint32_t cp = 0;
                int k = 0;
                while (k < 4 && i + (size_t)k < n && isxdigit((unsigned char)s[i + k])) { cp = cp * 16 + jv_hx(s[i + k]); k++; }
                i += (size_t)k;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= n && s[i] == '\\' && s[i + 1] == 'u') {
                    uint32_t lo = 0;
                    int m = 0;
                    while (m < 4 && isxdigit((unsigned char)s[i + 2 + m])) { lo = lo * 16 + jv_hx(s[i + 2 + m]); m++; }
                    if (m == 4 && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                }
                if (cp == 0) cp = 0xFFFD;
                if (!jv_pcp(d, cp)) return false;
                continue;
            }
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                default: c = e; break;
            }
        }
        if (!jv_pc(d, c)) return false;
    }
    if (i < n) i++;
    return jv_pc(d, 0);
}

static bool jv_bare(JDoc &d, const char *s, size_t n, size_t &i, bool key, uint32_t &off, size_t &len) {
    off = (uint32_t)d.plen;
    const size_t st = i;
    while (i < n) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == '}' || c == ']' || c == '{' || c == '[') break;
        if (key && (c == ':' || c == '=')) break;
        if (c == '/' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '*')) break;
        if (!jv_pc(d, c)) return false;
        i++;
    }
    len = i - st;
    return jv_pc(d, 0);
}

static bool jv_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static uint8_t jv_classify(const char *t) {
    if (jv_ieq(t, "true") || jv_ieq(t, "false")) return JT_BOOL;
    if (jv_ieq(t, "null") || jv_ieq(t, "undefined") || jv_ieq(t, "nil")) return JT_NULL;
    if (jv_ieq(t, "nan") || jv_ieq(t, "infinity") || jv_ieq(t, "-infinity") || jv_ieq(t, "+infinity") || jv_ieq(t, "inf")) return JT_NUM;
    const char c = t[0];
    if (isdigit((unsigned char)c) || ((c == '-' || c == '+' || c == '.') && (isdigit((unsigned char)t[1]) || t[1] == '.'))) return JT_NUM;
    return JT_STR;
}

static int jv_node(JDoc &d, uint8_t type, int parent, uint32_t key, uint32_t val, uint32_t vlen) {
    if (d.cnt >= JV_MAX_NODES) { d.trunc = true; return -1; }
    const int id = d.cnt++;
    JNode &x = d.n[id];
    memset(&x, 0, sizeof(x));
    x.type = type;
    x.parent = parent;
    x.first = x.last = x.next = -1;
    x.key = key;
    x.val = val;
    x.vlen = vlen;
    if (parent >= 0) {
        JNode &p = d.n[parent];
        x.idx = p.nchild++;
        x.depth = (uint16_t)(p.depth + 1);
        if (p.last >= 0) d.n[p.last].next = id; else p.first = id;
        p.last = id;
    }
    return id;
}

static bool jv_parse(JDoc &d, const char *s, size_t n) {
    size_t i = 0;
    if (n >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) i = 3;
    d.root = jv_node(d, JT_ARR, -1, JV_NONE, JV_NONE, 0);
    if (d.root < 0) return false;
    int stk[JV_MAX_DEPTH + 2];
    int sp = 0;
    stk[0] = d.root;
    while (!d.trunc) {
        jv_ws(s, n, i);
        if (i >= n) break;
        const int c = stk[sp];
        const char ch = s[i];
        if (ch == ',') { i++; continue; }
        if (ch == '}' || ch == ']') { i++; if (sp > 0) sp--; continue; }
        uint32_t koff = JV_NONE;
        if (d.n[c].type == JT_OBJ) {
            if (ch == '"' || ch == '\'' || ch == '`') {
                if (!jv_str(d, s, n, i, koff)) break;
            } else {
                size_t kl = 0;
                if (!jv_bare(d, s, n, i, true, koff, kl)) break;
                if (kl == 0) { i++; continue; }
            }
            jv_ws(s, n, i);
            if (i < n && (s[i] == ':' || s[i] == '=')) i++;
            jv_ws(s, n, i);
            if (i >= n || s[i] == ',' || s[i] == '}' || s[i] == ']') {   // a key without a value
                jv_node(d, JT_NULL, c, koff, JV_NONE, 0);
                continue;
            }
        }
        const char v = s[i];
        if (v == '{' || v == '[') {
            if (sp + 2 >= JV_MAX_DEPTH) { i++; continue; }
            const int id = jv_node(d, v == '{' ? JT_OBJ : JT_ARR, c, koff, JV_NONE, 0);
            if (id < 0) break;
            stk[++sp] = id;
            i++;
            continue;
        }
        if (v == '"' || v == '\'' || v == '`') {
            uint32_t vo = 0;
            if (!jv_str(d, s, n, i, vo)) break;
            jv_node(d, JT_STR, c, koff, vo, (uint32_t)strlen(d.pool + vo));
            continue;
        }
        uint32_t vo = 0;
        size_t vl = 0;
        if (!jv_bare(d, s, n, i, false, vo, vl)) break;
        if (vl == 0) { i++; continue; }
        jv_node(d, jv_classify(d.pool + vo), c, koff, vo, (uint32_t)vl);
    }
    return d.cnt > 1;
}

static int jv_disp_root(const JDoc &d) { return d.n[d.root].nchild == 1 ? d.n[d.root].first : d.root; }

// ---------------- printer (pretty / compact) ----------------
struct JOut { char *b; size_t n, cap; bool fail; };

static void jo_put(JOut &o, const char *s, size_t l) {
    if (o.fail) return;
    if (o.n + l + 1 > o.cap) {
        size_t nc = o.cap ? o.cap * 2 : 4096;
        while (nc < o.n + l + 1) nc *= 2;
        if (nc > ((size_t)4 << 20)) { o.fail = true; return; }
        char *nb = (char *)heap_caps_realloc(o.b, nc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) { o.fail = true; return; }
        o.b = nb;
        o.cap = nc;
    }
    memcpy(o.b + o.n, s, l);
    o.n += l;
    o.b[o.n] = 0;
}

static void jo_ch(JOut &o, char c) { jo_put(o, &c, 1); }

static void jo_q(JOut &o, const char *s) {
    jo_ch(o, '"');
    char tmp[64];
    size_t t = 0;
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        const char *rep = nullptr;
        char u[8];
        if (c == '"') rep = "\\\"";
        else if (c == '\\') rep = "\\\\";
        else if (c == '\n') rep = "\\n";
        else if (c == '\r') rep = "\\r";
        else if (c == '\t') rep = "\\t";
        else if (c < 0x20) { snprintf(u, sizeof(u), "\\u%04x", (unsigned)c); rep = u; }
        if (rep) {
            if (t) { jo_put(o, tmp, t); t = 0; }
            jo_put(o, rep, strlen(rep));
        } else {
            tmp[t++] = (char)c;
            if (t >= sizeof(tmp)) { jo_put(o, tmp, t); t = 0; }
        }
    }
    if (t) jo_put(o, tmp, t);
    jo_ch(o, '"');
}

static bool jv_json_num(const char *t) {
    const char *p = t;
    if (*p == '-') p++;
    if (!isdigit((unsigned char)*p)) return false;
    if (*p == '0' && isdigit((unsigned char)p[1])) return false;
    while (isdigit((unsigned char)*p)) p++;
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    return *p == 0;
}

static void jo_indent(JOut &o, int depth) {
    for (int i = 0; i < depth; i++) jo_put(o, "  ", 2);
}

// the subtree of `root` as JSON text in a new buffer (free() it); nullptr on failure
static char *jv_print(const JDoc &d, int root, bool pretty, size_t *outn) {
    JOut o;
    memset(&o, 0, sizeof(o));
    int n = root;
    int depth = 0;
    for (;;) {
        const JNode &x = d.n[n];
        if (pretty) jo_indent(o, depth);
        if (n != root && d.n[x.parent].type == JT_OBJ && x.key != JV_NONE) {
            jo_q(o, d.pool + x.key);
            if (pretty) jo_put(o, ": ", 2); else jo_ch(o, ':');
        }
        if (x.type == JT_OBJ || x.type == JT_ARR) {
            jo_ch(o, x.type == JT_OBJ ? '{' : '[');
            if (x.first < 0) {
                jo_ch(o, x.type == JT_OBJ ? '}' : ']');
            } else {
                if (pretty) jo_ch(o, '\n');
                n = x.first;
                depth++;
                continue;
            }
        } else if (x.type == JT_STR) {
            jo_q(o, d.pool + x.val);
        } else if (x.type == JT_NUM) {
            const char *t = d.pool + x.val;
            if (jv_json_num(t)) jo_put(o, t, strlen(t)); else jo_q(o, t);
        } else if (x.type == JT_BOOL) {
            const char *t = d.pool + x.val;
            if (t[0] == 't' || t[0] == 'T') jo_put(o, "true", 4); else jo_put(o, "false", 5);
        } else {
            jo_put(o, "null", 4);
        }
        for (;;) {
            if (n == root) { if (pretty) jo_ch(o, '\n'); goto done; }
            if (d.n[n].next >= 0) {
                jo_ch(o, ',');
                if (pretty) jo_ch(o, '\n');
                n = d.n[n].next;
                break;
            }
            n = d.n[n].parent;
            depth--;
            if (pretty) { jo_ch(o, '\n'); jo_indent(o, depth); }
            jo_ch(o, d.n[n].type == JT_OBJ ? '}' : ']');
        }
    }
done:
    if (o.fail) { free(o.b); return nullptr; }
    if (outn) *outn = o.n;
    return o.b;
}

bool k85_json_normalize(const char *src, size_t n, char **out, size_t *outn) {
    *out = nullptr;
    if (outn) *outn = 0;
    JDoc d;
    if (!jv_open(d, n)) return false;
    bool ok = jv_parse(d, src, n);
    if (ok) {
        size_t pn = 0;
        char *s = jv_print(d, jv_disp_root(d), false, &pn);
        if (s) { *out = s; if (outn) *outn = pn; } else ok = false;
    }
    jv_close(d);
    return ok;
}

// ---------------- small UI helpers ----------------
static void jv_wait_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void jv_msg(const char *s) {
    const int W = M5.Display.width(), H = M5.Display.height();
    M5.Display.fillRect(0, H / 2 - 14, W, 28, 0x000000);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xFFFFFF, 0x000000);
    M5.Display.setCursor(6, H / 2 - 4);
    M5.Display.print(s);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static inline int jv_cplen(unsigned char c) { return c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1)); }

// full-screen read-only text with wrapping: A = page down (wraps), hold A = page up, B / A+B = back
static void jv_text_view(const char *title, const char *txt) {
    const int W = M5.Display.width(), H = M5.Display.height();
    const uint32_t bg = k85_get_bg(), fg = k85_get_fg(), accent = k85_get_accent();
    const int cols = (W - 6) / 6, lh = 10, y0 = 14;
    size_t n = strlen(txt);
    if (n > 8000) {
        n = 8000;
        while (n > 0 && ((unsigned char)txt[n] & 0xC0) == 0x80) n--;
    }
    struct Ln { uint32_t off; uint16_t len; };
    const int MAXL = 800;
    Ln *ln = (Ln *)jv_alloc(sizeof(Ln) * MAXL);
    if (!ln) return;
    int nl = 0;
    size_t i = 0;
    while (i <= n && nl < MAXL) {
        const size_t st = i;
        int cp = 0;
        while (i < n && txt[i] != '\n' && cp < cols) {
            if (txt[i] == '\r') { i++; continue; }
            size_t l = (size_t)jv_cplen((unsigned char)txt[i]);
            if (i + l > n) l = n - i;
            i += l;
            cp++;
        }
        ln[nl].off = (uint32_t)st;
        ln[nl].len = (uint16_t)(i - st);
        nl++;
        if (i < n && txt[i] == '\n') i++;
        else if (i >= n) break;
    }
    int vis = (H - y0 - 2) / lh;
    if (vis < 1) vis = 1;
    int top = 0;
    bool dirty = true;
    while (true) {
        if (dirty) {
            dirty = false;
            M5.Display.fillScreen(bg);
            M5.Display.setTextSize(1);
            char hb[40];
            k85_u8trunc(hb, sizeof(hb), title, 30);
            M5.Display.setTextColor(accent, bg);
            M5.Display.setCursor(2, 2);
            M5.Display.print(hb);
            M5.Display.setTextColor(fg, bg);
            for (int r = 0; r < vis && top + r < nl; r++) {
                char tmp[200];
                size_t len = ln[top + r].len;
                if (len > sizeof(tmp) - 1) len = sizeof(tmp) - 1;
                memcpy(tmp, txt + ln[top + r].off, len);
                tmp[len] = 0;
                M5.Display.setCursor(2, y0 + r * lh);
                M5.Display.print(tmp);
            }
            if (nl > vis) {
                int bh = H * vis / nl;
                if (bh < 6) bh = 6;
                M5.Display.fillRect(W - 3, (H - bh) * top / (nl - vis), 3, bh, accent);
            }
        }
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            if (top + vis >= nl) top = 0;
            else { top += vis - 1; if (top > nl - vis) top = nl - vis; if (top < 0) top = 0; }
            dirty = true;
        }
        const int bk = k85_a_hold_back(2);
        if (bk > 0) {
            top -= bk * (vis - 1);
            if (top < 0) top = 0;
            dirty = true;
        }
        if (k85_btn_b_pressed()) { k85_wake_screen(); break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    free(ln);
}

// ---------------- tree view ----------------
static int jv_rebuild(JDoc &d, int dr) {
    int nv = 0;
    int n = dr;
    for (;;) {
        if (nv < JV_MAX_NODES) d.vis[nv++] = n;
        const JNode &x = d.n[n];
        if (x.type <= JT_ARR && x.exp && x.first >= 0) { n = x.first; continue; }
        for (;;) {
            if (n == dr) return nv;
            if (d.n[n].next >= 0) { n = d.n[n].next; break; }
            n = d.n[n].parent;
        }
    }
}

static int jv_locate(const JDoc &d, int nv, int id) {
    for (int i = 0; i < nv; i++) {
        if (d.vis[i] == id) return i;
    }
    return -1;
}

static void jv_path(const JDoc &d, int id, char *out, size_t cap) {
    int chain[JV_MAX_DEPTH + 4];
    int cn = 0;
    for (int p = id; p >= 0 && p != d.root && cn < JV_MAX_DEPTH + 4; p = d.n[p].parent) chain[cn++] = p;
    size_t o = 0;
    out[0] = 0;
    auto add = [&](const char *s) {
        while (*s && o + 1 < cap) out[o++] = *s++;
        out[o] = 0;
    };
    add("$");
    for (int k = cn - 1; k >= 0; k--) {
        const JNode &x = d.n[chain[k]];
        const JNode &p = d.n[x.parent];
        char t[24];
        if (x.parent == d.root) {
            if (p.nchild > 1) { snprintf(t, sizeof(t), "[%u]", (unsigned)x.idx); add(t); }
        } else if (p.type == JT_OBJ && x.key != JV_NONE) {
            const char *kk = d.pool + x.key;
            bool ident = kk[0] != 0 && !isdigit((unsigned char)kk[0]);
            for (const char *q = kk; *q; q++) {
                if (!isalnum((unsigned char)*q) && *q != '_') { ident = false; break; }
            }
            if (ident) { add("."); add(kk); } else { add("[\""); add(kk); add("\"]"); }
        } else {
            snprintf(t, sizeof(t), "[%u]", (unsigned)x.idx);
            add(t);
        }
    }
}

static bool jv_has_ci(const char *hay, const char *needle) {
    const size_t nl = strlen(needle);
    if (nl == 0) return true;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
        if (!hay[i]) return false;
    }
    return false;
}

static void jv_cell(int &x, int y, const char *src, uint32_t col, uint32_t bgc, int &cols) {
    if (cols <= 0 || !src || !*src) return;
    char clean[128];
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < sizeof(clean); i++) {
        const unsigned char c = (unsigned char)src[i];
        clean[o++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (char)c;
    }
    size_t k = o;
    while (k > 0 && ((unsigned char)clean[k - 1] & 0xC0) == 0x80) k--;
    if (k > 0) {
        const size_t need = (size_t)jv_cplen((unsigned char)clean[k - 1]);
        if (o - (k - 1) < need) o = k - 1;
    }
    clean[o] = 0;
    char buf[128];
    const int len = k85_u8len(clean);
    if (len > cols) {
        if (cols > 2) {
            k85_u8trunc(buf, sizeof(buf), clean, cols - 2);
            strncat(buf, "..", sizeof(buf) - strlen(buf) - 1);
        } else {
            k85_u8trunc(buf, sizeof(buf), clean, cols);
        }
    } else {
        snprintf(buf, sizeof(buf), "%s", clean);
    }
    M5.Display.setTextColor(col, bgc);
    M5.Display.setCursor(x, y);
    M5.Display.print(buf);
    const int w = k85_u8len(buf);
    x += w * 6;
    cols -= w;
}

static void jv_row(const JDoc &d, int id, int dr, int y, bool sel, int W, uint32_t bg, uint32_t accent) {
    const JNode &x = d.n[id];
    const uint32_t rb = sel ? accent : bg;
    if (sel) M5.Display.fillRect(0, y - 1, W, 10, accent);
    int rel = (int)x.depth - (int)d.n[dr].depth;
    if (rel < 0) rel = 0;
    if (rel > 8) rel = 8;
    int px = 2 + rel * 6;
    int cols = (W - px) / 6;
    const uint32_t blk = 0x000000;
    const char *mk = "  ";
    if (x.type <= JT_ARR) mk = x.first < 0 ? "o " : (x.exp ? "- " : "+ ");
    jv_cell(px, y, mk, sel ? blk : 0xFFD54F, rb, cols);
    if (id != dr && x.parent >= 0) {
        const JNode &p = d.n[x.parent];
        if (p.type == JT_OBJ && x.key != JV_NONE) {
            jv_cell(px, y, d.pool + x.key, sel ? blk : 0x66CCFF, rb, cols);
            jv_cell(px, y, ": ", sel ? blk : 0x7F8FB5, rb, cols);
        } else if (p.type == JT_ARR) {
            char ib[16];
            snprintf(ib, sizeof(ib), "[%u] ", (unsigned)x.idx);
            jv_cell(px, y, ib, sel ? blk : 0x7F8FB5, rb, cols);
        }
    }
    char vb[140];
    uint32_t vc = 0xFFFFFF;
    switch (x.type) {
        case JT_OBJ:
            if (id == d.root) snprintf(vb, sizeof(vb), "(%u values)", (unsigned)x.nchild);
            else snprintf(vb, sizeof(vb), "{%u}", (unsigned)x.nchild);
            vc = 0xFFFFFF;
            break;
        case JT_ARR:
            if (id == d.root) snprintf(vb, sizeof(vb), "(%u values)", (unsigned)x.nchild);
            else snprintf(vb, sizeof(vb), "[%u]", (unsigned)x.nchild);
            vc = 0xFFFFFF;
            break;
        case JT_STR: snprintf(vb, sizeof(vb), "\"%.100s\"", d.pool + x.val); vc = 0xA5D6A7; break;
        case JT_NUM: snprintf(vb, sizeof(vb), "%.100s", d.pool + x.val); vc = 0xFFCC80; break;
        case JT_BOOL: snprintf(vb, sizeof(vb), "%.10s", d.pool + x.val); vc = 0xCE93D8; break;
        default: snprintf(vb, sizeof(vb), "null"); vc = 0x9E9E9E; break;
    }
    jv_cell(px, y, vb, sel ? blk : vc, rb, cols);
}

static const char *jv_tail(const char *s, int keep) {
    int skip = k85_u8len(s) - keep;
    while (skip > 0 && *s) {
        s++;
        while (((unsigned char)*s & 0xC0) == 0x80) s++;
        skip--;
    }
    return s;
}

static void jv_draw(const JDoc &d, int nvis, int sel, int top, int rows, int dr, const char *title) {
    const int W = M5.Display.width(), H = M5.Display.height();
    const uint32_t bg = k85_get_bg(), accent = k85_get_accent();
    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    char tb[32], hb[64];
    k85_u8trunc(tb, sizeof(tb), title, 22);
    snprintf(hb, sizeof(hb), "%s  %d/%d", tb, sel + 1, nvis);
    M5.Display.setTextColor(accent, bg);
    M5.Display.setCursor(2, 2);
    M5.Display.print(hb);
    for (int r = 0; r < rows && top + r < nvis; r++) jv_row(d, d.vis[top + r], dr, 13 + r * 10, top + r == sel, W, bg, accent);
    char pb[160];
    jv_path(d, d.vis[sel], pb, sizeof(pb));
    const int pc = (W - 4) / 6;
    const char *ps = (k85_u8len(pb) > pc) ? jv_tail(pb, pc - 1) : pb;
    M5.Display.setTextColor(0x7F8FB5, bg);
    M5.Display.setCursor(2, H - 10);
    if (ps != pb) M5.Display.print("~");
    M5.Display.print(ps);
}

static void jv_save_pretty(const JDoc &d, int dr, const char *title, const char *path) {
    char out[300];
    if (path) {
        snprintf(out, sizeof(out), "%s", path);
        char *dot = strrchr(out, '.');
        const char *slash = strrchr(out, '/');
        if (dot && (!slash || dot > slash)) *dot = 0;
        const size_t l = strlen(out);
        snprintf(out + l, sizeof(out) - l, ".pretty.json");
    } else {
        char base[48];
        size_t o = 0;
        for (const char *p = title; *p && o + 1 < sizeof(base); p++) base[o++] = (isalnum((unsigned char)*p) || *p == '_' || *p == '-') ? *p : '_';
        base[o] = 0;
        snprintf(out, sizeof(out), "/littlefs/%s.pretty.json", base[0] ? base : "data");
    }
    if (k85_is_blocked_path(out)) { jv_msg("Not allowed"); return; }
    size_t pn = 0;
    char *s = jv_print(d, dr, true, &pn);
    if (!s) { jv_msg("Out of memory"); return; }
    FILE *f = fopen(out, "wb");
    bool ok = f && fwrite(s, 1, pn, f) == pn;
    if (f && fclose(f) != 0) ok = false;
    free(s);
    const char *nm = strrchr(out, '/');
    char m[80];
    snprintf(m, sizeof(m), ok ? "Saved %.40s" : "Write failed %.30s", nm ? nm + 1 : out);
    jv_msg(m);
}

static void jv_view(JDoc &d, const char *title, const char *path) {
    const int H = M5.Display.height();
    int rows = (H - 24) / 10;
    if (rows < 3) rows = 3;
    const int dr = jv_disp_root(d);
    for (int i = 0; i < d.cnt; i++) d.n[i].exp = 0;
    d.n[dr].exp = 1;
    if (d.cnt <= 400) {
        for (int i = 0; i < d.cnt; i++) {
            if (d.n[i].type <= JT_ARR) d.n[i].exp = 1;
        }
    }
    int nvis = jv_rebuild(d, dr);
    int sel = 0, top = 0;
    char last_q[48] = "";
    bool dirty = true, chord = false;

    auto reb = [&](int keep) {
        nvis = jv_rebuild(d, dr);
        const int p = jv_locate(d, nvis, keep);
        if (p >= 0) sel = p;
        if (sel >= nvis) sel = nvis - 1;
        if (sel < 0) sel = 0;
    };
    auto find_from = [&](int from) -> int {
        for (int k = 1; k <= d.cnt; k++) {
            const int id = (from + k) % d.cnt;
            if (id == dr) continue;
            int p = id;
            bool inside = false;
            while (p >= 0) {
                if (p == dr) { inside = true; break; }
                p = d.n[p].parent;
            }
            if (!inside) continue;
            const JNode &x = d.n[id];
            if (x.key != JV_NONE && jv_has_ci(d.pool + x.key, last_q)) return id;
            if (x.type > JT_ARR && x.val != JV_NONE && jv_has_ci(d.pool + x.val, last_q)) return id;
        }
        return -1;
    };
    auto goto_match = [&]() {
        const int found = find_from(d.vis[sel]);
        if (found < 0) { jv_msg("Not found"); return; }
        for (int p = d.n[found].parent; p >= 0; p = d.n[p].parent) d.n[p].exp = 1;
        reb(found);
    };

    while (true) {
        if (sel < top) top = sel;
        else if (sel >= top + rows) top = sel - rows + 1;
        if (dirty) {
            jv_draw(d, nvis, sel, top, rows, dr, title);
            dirty = false;
        }
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        if (k85_check_ab_hold()) chord = true;
        if (chord) {
            if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) chord = false;
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            sel = (sel + 1) % nvis;
            dirty = true;
        }
        const int bk = k85_a_hold_back(2);
        if (bk > 0) {
            sel = ((sel - bk) % nvis + nvis) % nvis;
            dirty = true;
        }
        const bool b_click = M5.BtnB.wasClicked() && !M5.BtnA.isPressed();
        const bool b_hold = M5.BtnB.wasHold() && !M5.BtnA.isPressed();
        if (b_click) {
            k85_wake_screen();
            const int id = d.vis[sel];
            JNode &x = d.n[id];
            if (x.type <= JT_ARR) {
                if (x.first >= 0) { x.exp = x.exp ? 0 : 1; reb(id); }
            } else {
                char pb[160];
                jv_path(d, id, pb, sizeof(pb));
                jv_text_view(pb, d.pool + x.val);
            }
            dirty = true;
        }
        if (b_hold) {
            k85_wake_screen();
            const char *it[14];
            int act[14];
            int na = 0;
            it[na] = "Find text"; act[na++] = 1;
            if (last_q[0]) { it[na] = "Find next"; act[na++] = 2; }
            it[na] = "Copy value"; act[na++] = 3;
            it[na] = "Copy path"; act[na++] = 4;
            it[na] = "Expand all"; act[na++] = 5;
            it[na] = "Collapse all"; act[na++] = 6;
            it[na] = "Go to top"; act[na++] = 7;
            it[na] = "Jump +20"; act[na++] = 8;
            it[na] = "Jump -20"; act[na++] = 9;
            it[na] = "Save formatted"; act[na++] = 10;
            if (path) { it[na] = "Run as game"; act[na++] = 11; }
            it[na] = "Close menu"; act[na++] = 0;
            const int r = k85_run_list_menu("JSON", it, na, nullptr);
            const int a = (r >= 0 && r < na) ? act[r] : 0;
            const int cur = d.vis[sel];
            if (a == 1) {
                if (k85_text_input("Find:", last_q, last_q, sizeof(last_q)) && last_q[0]) goto_match();
            } else if (a == 2) {
                goto_match();
            } else if (a == 3) {
                const JNode &x = d.n[cur];
                if (x.type > JT_ARR) {
                    k85_clip_push(d.pool + x.val, strlen(d.pool + x.val));
                } else {
                    size_t pn = 0;
                    char *s = jv_print(d, cur, false, &pn);
                    if (s) { k85_clip_push(s, pn); free(s); }
                }
                jv_msg("Copied");
            } else if (a == 4) {
                char pb[160];
                jv_path(d, cur, pb, sizeof(pb));
                k85_clip_push(pb, strlen(pb));
                jv_msg("Copied");
            } else if (a == 5) {
                for (int i = 0; i < d.cnt; i++) {
                    if (d.n[i].type <= JT_ARR) d.n[i].exp = 1;
                }
                reb(cur);
            } else if (a == 6) {
                for (int i = 0; i < d.cnt; i++) d.n[i].exp = 0;
                d.n[dr].exp = 1;
                reb(dr);
                sel = 0;
            } else if (a == 7) {
                sel = 0;
            } else if (a == 8) {
                sel += 20;
                if (sel >= nvis) sel = nvis - 1;
            } else if (a == 9) {
                sel -= 20;
                if (sel < 0) sel = 0;
            } else if (a == 10) {
                jv_save_pretty(d, dr, title, path);
            } else if (a == 11) {
                k85_json_run_file(path);
            }
            dirty = true;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static void jv_run(const char *src, size_t n, const char *title, const char *path) {
    JDoc d;
    if (!jv_open(d, n)) { k85_show_message("Out of memory\nA+B=back"); jv_wait_exit(); return; }
    if (!jv_parse(d, src, n)) {
        jv_close(d);
        jv_text_view(title, src);   // nothing JSON-like: show the raw text
        return;
    }
    jv_view(d, title, path);
    jv_close(d);
}

void k85_json_view_buffer(const char *buf, size_t n, const char *title) {
    if (!buf) return;
    jv_run(buf, n, title ? title : "JSON", nullptr);
}

void k85_json_view_file(const char *path) {
    if (!path) return;
    if (k85_is_blocked_path(path)) { k85_show_message("Protected file\nA+B=back"); jv_wait_exit(); return; }
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) { k85_show_message("Cannot open file\nA+B=back"); jv_wait_exit(); return; }
    if (st.st_size > JV_MAX_FILE) { k85_show_message("File too large\n(max 768 KB)\nA+B=back"); jv_wait_exit(); return; }
    const size_t sz = (size_t)st.st_size;
    char *buf = (char *)jv_alloc(sz + 1);
    if (!buf) { k85_show_message("Out of memory\nA+B=back"); jv_wait_exit(); return; }
    FILE *f = fopen(path, "rb");
    if (!f) { free(buf); k85_show_message("Cannot open file\nA+B=back"); jv_wait_exit(); return; }
    const size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = 0;
    const char *nm = strrchr(path, '/');
    jv_run(buf, rd, nm ? nm + 1 : path, path);
    free(buf);
}