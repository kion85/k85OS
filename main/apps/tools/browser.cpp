// K85BROWSER: text-mode web browser (main menu -> Browser)
// HTML -> plain text (+ clickable links), Cyrillic/Latin-ext transliterated to ASCII
// (the bitmap font is ASCII only). A = next link / scroll, hold A = previous,
// B = open link, hold B = menu, A+B = back / exit.
#include "browser.h"
#include "esp_attr.h"

#ifndef K85_HOST_TEST
#include "common.h"
#include "theme.h"
#include "input.h"
#include "text_input.h"
#include "list_menu.h"
#include "wifi.h"
#include "heavy_lock.h"
#include "power.h"
#include "log.h"
#include "config.h"
#include "fonts.h"
#include "fs_guard.h"
#include <sys/stat.h>
#include "M5Unified.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdint>

#define BR_RAW_MAX    262144
#define BR_TXT_CAP    150000
#define BR_MAX_LINKS  300
#define BR_MAX_SPANS  700
#define BR_IMG_MAX    393216
#define BR_HINT       "hB=menu"
enum { ST_NORM = 0, ST_H1, ST_H2, ST_DIM, ST_URL, ST_CODE };   // per-character text style
enum { LK_URL = 0, LK_IMG = 1, LK_FORM = 2 };                    // link kinds (special links start with byte 1)
#define BR_URL_MAX    300
#define BR_HIST_MAX   10
#define BR_BM_MAX     12
#define BR_BM_FILE    "/littlefs/browser_bm.txt"

static void *br_alloc(size_t n) {
#ifdef K85_HOST_TEST
    return calloc(1, n);
#else
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(1, n, MALLOC_CAP_8BIT);
    return p;
#endif
}

struct Span { uint32_t a, b; int16_t link; };

struct Page {
    char url[BR_URL_MAX];
    char title[40];
    char host[24]; uint16_t hue;
    char *txt; uint8_t *sty; uint32_t len;
    Span *spans; int nspans;
    char **links; int nlinks;
    uint32_t *loff; uint16_t *llen; int nlines, cols;
    int top, cur;
    bool trunc;
};

static void page_free(Page *p) {
    if (!p) return;
    for (int i = 0; i < p->nlinks; i++) free(p->links[i]);
    free(p->links); free(p->spans); free(p->txt); free(p->sty); free(p->loff); free(p->llen);
    free(p);
}

// ======================= string helpers =======================
static bool starts_ci(const char *s, const char *pre) {
    while (*pre) { if (tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return false; s++; pre++; }
    return true;
}
static size_t find_ci(const char *h, size_t n, size_t from, const char *pat) {
    size_t pl = strlen(pat);
    for (size_t i = from; i + pl <= n; i++) {
        size_t k = 0;
        while (k < pl && tolower((unsigned char)h[i + k]) == tolower((unsigned char)pat[k])) k++;
        if (k == pl) return i;
    }
    return (size_t)-1;
}

// ======================= charset -> ASCII =======================
static const char LAT1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
static const char LATA[] = "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh" "IiIiIiIiIi" "Jj" "Jj" "Kkk"
                           "LlLlLlLlLl" "NnNnNnnNn" "OoOoOoOo" "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu" "Ww" "YyY" "ZzZzZz" "s";

static const char *cyr(uint32_t cp) {
    static const char *const U[32] = {"A","B","V","G","D","E","Zh","Z","I","Y","K","L","M","N","O","P","R","S","T","U","F","Kh","Ts","Ch","Sh","Sch","\"","Y","'","E","Yu","Ya"};
    static const char *const L[32] = {"a","b","v","g","d","e","zh","z","i","y","k","l","m","n","o","p","r","s","t","u","f","kh","ts","ch","sh","sch","\"","y","'","e","yu","ya"};
    if (cp >= 0x410 && cp <= 0x42F) return U[cp - 0x410];
    if (cp >= 0x430 && cp <= 0x44F) return L[cp - 0x430];
    switch (cp) {
        case 0x401: return "Yo"; case 0x451: return "yo";
        case 0x404: return "Ye"; case 0x454: return "ye";
        case 0x406: return "I";  case 0x456: return "i";
        case 0x407: return "Yi"; case 0x457: return "yi";
        case 0x490: return "G";  case 0x491: return "g";
    }
    return nullptr;
}

// returns ASCII replacement, "" for invisible chars, nullptr for unknown
static const char *cp_ascii(uint32_t cp, char *t) {
    if (cp < 0x80) { t[0] = (char)cp; t[1] = 0; return t; }
    if (cp >= 0xC0 && cp <= 0xFF) { t[0] = LAT1[cp - 0xC0]; t[1] = 0; return t; }
    if (cp >= 0x100 && cp <= 0x17F) { t[0] = LATA[cp - 0x100]; t[1] = 0; return t; }
    if (cp >= 0x300 && cp <= 0x36F) return "";
    if (cp >= 0x200B && cp <= 0x200F) return "";
    if (cp >= 0x202A && cp <= 0x202E) return "";
    if (cp == 0xAD || cp == 0xFEFF || cp == 0x2060) return "";
    const char *c = cyr(cp);
    if (c) return c;
    switch (cp) {
        case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return " ";
        case 0x2018: case 0x2019: case 0x201A: return "'";
        case 0x201C: case 0x201D: case 0x201E: return "\"";
        case 0x2013: case 0x2014: case 0x2212: return "-";
        case 0x2026: return "...";
        case 0x2022: case 0xB7: return "*";
        case 0xAB: return "<<";
        case 0xBB: return ">>";
        case 0xA9: return "(c)";
        case 0xAE: return "(R)";
        case 0x2122: return "TM";
        case 0x20AC: return "EUR";
        case 0xB0: return "deg";
        case 0x2192: return "->";
        case 0x2190: return "<-";
        case 0x2116: return "No";
    }
    return nullptr;
}

static uint32_t cp1251_to_cp(unsigned char c) {
    if (c >= 0xC0) return 0x410 + (c - 0xC0);
    switch (c) {
        case 0xA8: return 0x401; case 0xB8: return 0x451;
        case 0xA0: return 0xA0;  case 0x96: return 0x2013; case 0x97: return 0x2014;
        case 0x85: return 0x2026; case 0x91: return 0x2018; case 0x92: return 0x2019;
        case 0x93: return 0x201C; case 0x94: return 0x201D; case 0x95: return 0x2022;
        case 0xAB: return 0xAB;  case 0xBB: return 0xBB;  case 0xB9: return 0x2116;
    }
    return 0xFFFF;
}

// ======================= text output =======================
struct Out {
    char *buf; uint32_t len, cap; bool trunc;
    bool sp; int nl; bool unk; bool pre;
    Page *pg; int open; uint32_t span_a; const char *base;
    uint8_t *sty; uint8_t cur;
};

static inline void o_put(Out &o, char c) {
    if (o.len + 2 >= o.cap) { o.trunc = true; return; }
    if (o.sty) o.sty[o.len] = o.cur;
    o.buf[o.len++] = c; o.buf[o.len] = 0;
}

static void o_ch(Out &o, char c) {
    if (c == '\r') return;
    if (c == '\n') {
        if (o.pre) { o_put(o, '\n'); o.nl++; o.sp = true; }
        else if (!o.sp) { o_put(o, ' '); o.sp = true; }
        return;
    }
    if (c == ' ' || c == '\t') {
        if (o.pre) { o_put(o, ' '); o.sp = true; return; }
        if (!o.sp) { o_put(o, ' '); o.sp = true; }
        return;
    }
    if ((unsigned char)c < 32 || (unsigned char)c > 126) return;
    o_put(o, c); o.sp = false; o.nl = 0; o.unk = false;
}

static void o_nl(Out &o, int n) {
    if (o.len == 0) return;
    while (o.len > 0 && o.buf[o.len - 1] == ' ') o.len--;
    o.buf[o.len] = 0;
    while (o.nl < n) { o_put(o, '\n'); o.nl++; }
    o.sp = true;
}

static void emit_cp(Out &o, uint32_t cp) {
    char t[4];
    const char *s = cp_ascii(cp, t);
    if (!s) { if (!o.unk) { o_ch(o, '?'); o.unk = true; } return; }
    for (; *s; s++) o_ch(o, *s);
}

struct Ent { const char *n; uint32_t cp; };
static const Ent ENTS[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026}, {"laquo", 0xAB}, {"raquo", 0xBB},
    {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"bull", 0x2022}, {"middot", 0xB7},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"euro", 0x20AC},
    {"rarr", 0x2192}, {"larr", 0x2190}, {"times", 0xD7}, {"deg", 0xB0}, {"shy", 0xAD},
};

// s points right after '&'. Returns consumed length (with ';'), 0 if not an entity.
static size_t ent_decode(const char *s, size_t n, uint32_t *cp) {
    if (n < 2) return 0;
    if (s[0] == '#') {
        size_t i = 1; bool hex = false;
        if (i < n && (s[i] == 'x' || s[i] == 'X')) { hex = true; i++; }
        size_t st = i; uint32_t v = 0;
        while (i < n && i - st < 8) {
            char c = s[i]; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && (c | 32) >= 'a' && (c | 32) <= 'f') d = (c | 32) - 'a' + 10;
            else break;
            v = v * (hex ? 16 : 10) + d; i++;
        }
        if (i == st) return 0;
        if (i < n && s[i] == ';') i++;
        *cp = v; return i;
    }
    size_t i = 0;
    while (i < n && i < 10 && isalnum((unsigned char)s[i])) i++;
    if (i == 0 || i >= n || s[i] != ';') return 0;
    for (size_t k = 0; k < sizeof(ENTS) / sizeof(ENTS[0]); k++)
        if (strlen(ENTS[k].n) == i && !strncmp(ENTS[k].n, s, i)) { *cp = ENTS[k].cp; return i + 1; }
    return 0;
}

static void feed_text(Out &o, const char *s, size_t n, bool cp1251) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        if (c == '&') {
            size_t k = ent_decode(s + i + 1, n - i - 1, &cp);
            if (k) { i += 1 + k; emit_cp(o, cp); } else { o_ch(o, '&'); i++; }
            continue;
        }
        if (c < 0x80) { o_ch(o, (char)c); i++; continue; }
        if (cp1251) { i++; emit_cp(o, cp1251_to_cp(c)); continue; }
        int need = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
        bool ok = need > 0 && i + need < n;
        cp = (need == 1) ? (c & 0x1F) : (need == 2) ? (c & 0x0F) : (c & 0x07);
        if (ok) for (int k = 1; k <= need; k++) {
            unsigned char b = (unsigned char)s[i + k];
            if ((b & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (!ok) { i++; emit_cp(o, 0xFFFF); }
        else { i += need + 1; emit_cp(o, cp); }
    }
}

// ======================= URLs =======================
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 32) >= 'a' && (c | 32) <= 'f') return (c | 32) - 'a' + 10;
    return -1;
}

static void url_encode(const char *in, char *out, size_t sz) {
    size_t o = 0;
    for (; *in && o + 4 < sz; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else { snprintf(out + o, 4, "%%%02X", c); o += 3; }
    }
    out[o] = 0;
}

static void url_norm(char *u) {
    char *s = strstr(u, "://");
    if (!s) return;
    char *path = strchr(s + 3, '/');
    if (!path) return;
    char *q = path;
    while (*q && *q != '?') q++;
    static char tail[BR_URL_MAX];
    snprintf(tail, sizeof(tail), "%s", q);
    *q = 0;
    static char out[BR_URL_MAX * 2];
    size_t ol = 0;
    const char *p = path;
    while (*p) {
        const char *e = p + 1;
        while (*e && *e != '/') e++;
        size_t sl = (size_t)(e - (p + 1));
        if (sl == 1 && p[1] == '.') { /* skip */ }
        else if (sl == 2 && p[1] == '.' && p[2] == '.') {
            while (ol > 0 && out[ol - 1] != '/') ol--;
            if (ol > 0) ol--;
        } else {
            if (ol + (size_t)(e - p) >= sizeof(out)) break;
            memcpy(out + ol, p, (size_t)(e - p)); ol += (size_t)(e - p);
        }
        p = e;
    }
    if (ol == 0) out[ol++] = '/';
    memcpy(path, out, ol);
    strcpy(path + ol, tail);
}

static bool url_resolve(const char *base, const char *ref, char *out, size_t sz) {
    while (*ref == ' ' || *ref == '\t' || *ref == '\n' || *ref == '\r') ref++;
    size_t rl = strlen(ref);
    while (rl > 0 && isspace((unsigned char)ref[rl - 1])) rl--;
    if (rl == 0 || ref[0] == '#') return false;
    static char r[BR_URL_MAX];      // static: keeps the UI task stack small (single-threaded use)
    if (rl >= sizeof(r)) return false;
    memcpy(r, ref, rl); r[rl] = 0;
    char *hash = strchr(r, '#'); if (hash) *hash = 0;
    if (!r[0]) return false;

    static char tmp[BR_URL_MAX * 2];
    if (starts_ci(r, "http://") || starts_ci(r, "https://") || (starts_ci(r, "file://") && starts_ci(base, "file://"))) {  // K85JV1
        snprintf(tmp, sizeof(tmp), "%s", r);
    } else {
        const char *c = r;
        while (isalnum((unsigned char)*c) || *c == '+' || *c == '-' || *c == '.') c++;
        if (*c == ':' && c > r) return false;               // mailto:, javascript:, data: ...
        const char *se = strstr(base, "://");
        if (!se) return false;
        const char *pe = se + 3;
        while (*pe && *pe != '/' && *pe != '?' && *pe != '#') pe++;
        int scheme_len = (int)(se - base), origin_len = (int)(pe - base);
        if (r[0] == '/' && r[1] == '/') snprintf(tmp, sizeof(tmp), "%.*s:%s", scheme_len, base, r);
        else if (r[0] == '/') snprintf(tmp, sizeof(tmp), "%.*s%s", origin_len, base, r);
        else if (r[0] == '?') {
            const char *q = pe; while (*q && *q != '?' && *q != '#') q++;
            snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(q - base), base, r);
        } else {
            const char *q = pe, *last = nullptr;
            while (*q && *q != '?' && *q != '#') { if (*q == '/') last = q; q++; }
            if (!last) snprintf(tmp, sizeof(tmp), "%.*s/%s", origin_len, base, r);
            else snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(last - base) + 1, base, r);
        }
    }
    url_norm(tmp);

    // DuckDuckGo result links are redirects (//duckduckgo.com/l/?uddg=<encoded>): go straight to the target
    const char *k = strstr(tmp, "duckduckgo.com/l/?");
    if (k) {
        const char *u = strstr(k, "uddg=");
        if (u) {
            u += 5;
            static char dec[BR_URL_MAX]; size_t d = 0;
            while (*u && *u != '&' && d + 1 < sizeof(dec)) {
                if (*u == '%' && hexv(u[1]) >= 0 && hexv(u[2]) >= 0) { dec[d++] = (char)(hexv(u[1]) * 16 + hexv(u[2])); u += 3; }
                else if (*u == '+') { dec[d++] = ' '; u++; }
                else dec[d++] = *u++;
            }
            dec[d] = 0;
            if (starts_ci(dec, "http://") || starts_ci(dec, "https://")) snprintf(tmp, sizeof(tmp), "%s", dec);
        }
    }
    if (strlen(tmp) >= sz) return false;
    strcpy(out, tmp);
    return true;
}

// ======================= HTML =======================
static bool attr_get(const char *t, size_t n, const char *name, char *out, size_t osz, bool novalue = false) {
    size_t i = 0;
    while (i < n && !isspace((unsigned char)t[i])) i++;          // skip tag name
    size_t nl = strlen(name);
    while (i < n) {
        while (i < n && (isspace((unsigned char)t[i]) || t[i] == '/')) i++;
        size_t ns = i;
        while (i < n && !isspace((unsigned char)t[i]) && t[i] != '=' && t[i] != '/') i++;
        size_t ne = i;
        while (i < n && isspace((unsigned char)t[i])) i++;
        char *dst = nullptr; size_t vs = 0, ve = 0; bool has = false;
        if (i < n && t[i] == '=') {
            i++;
            while (i < n && isspace((unsigned char)t[i])) i++;
            if (i < n && (t[i] == '"' || t[i] == '\'')) {
                char q = t[i++]; vs = i;
                while (i < n && t[i] != q) i++;
                ve = i; if (i < n) i++;
            } else {
                vs = i;
                while (i < n && !isspace((unsigned char)t[i])) i++;
                ve = i;
            }
            has = true;
        }
        (void)dst;
        if (ne - ns == nl && starts_ci(t + ns, name) && (has || novalue)) {
            size_t L = ve - vs; if (L >= osz) L = osz - 1;
            memcpy(out, t + vs, L); out[L] = 0;
            // minimal unescape for URLs/alt text
            char *w = out;
            for (const char *r = out; *r; ) {
                if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; }
                else if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; }
                else if (!strncmp(r, "&#39;", 5)) { *w++ = '\''; r += 5; }
                else *w++ = *r++;
            }
            *w = 0;
            return true;
        }
    }
    return false;
}

static int add_link(Page *pg, const char *url) {
    if (pg->nlinks > 0 && !strcmp(pg->links[pg->nlinks - 1], url)) return pg->nlinks - 1;
    if (pg->nlinks >= BR_MAX_LINKS) return -1;
    size_t L = strlen(url);
    char *c = (char *)br_alloc(L + 1);
    if (!c) return -1;
    memcpy(c, url, L + 1);
    pg->links[pg->nlinks] = c;
    return pg->nlinks++;
}

static void close_link(Out &o) {
    if (o.open < 0) return;
    Page *pg = o.pg;
    uint32_t a = o.span_a, b = o.len;
    if (a < b && b <= o.cap) {
        while (b > a && (o.buf[b - 1] == ' ' || o.buf[b - 1] == '\n')) b--;
        while (a < b && (o.buf[a] == ' ' || o.buf[a] == '\n')) a++;
        if (a < b) {
            if (pg->nspans > 0 && pg->spans[pg->nspans - 1].link == o.open && a - pg->spans[pg->nspans - 1].b <= 1) {
                pg->spans[pg->nspans - 1].b = b;
            } else if (pg->nspans < BR_MAX_SPANS) {
                pg->spans[pg->nspans++] = Span{a, b, (int16_t)o.open};
            }
        }
    }
    o.open = -1;
}

static bool is_skip(const char *n) {
    static const char *const S[] = {"script", "style", "noscript", "svg", "template", "iframe", "select"};
    for (size_t i = 0; i < sizeof(S) / sizeof(S[0]); i++) if (!strcmp(n, S[i])) return true;
    return false;
}

static bool in_list(const char *n, const char *const *l, size_t cnt) {
    for (size_t i = 0; i < cnt; i++) if (!strcmp(n, l[i])) return true;
    return false;
}

static inline int link_kind(const char *l) { return l[0] == 1 ? (l[1] == 'I' ? LK_IMG : LK_FORM) : LK_URL; }

// "en.m.wikipedia.org" -> "wikipedia.org"
static void host_short(const char *url, char *out, size_t sz) {
    const char *s = strstr(url, "://"); s = s ? s + 3 : url;
    size_t L = 0; while (s[L] && s[L] != '/' && s[L] != ':' && s[L] != '?') L++;
    const char *st = s; int dots = 0;
    for (size_t i = L; i > 0; i--) if (s[i - 1] == '.' && ++dots == 2) { st = s + i; break; }
    size_t k = L - (size_t)(st - s); if (k >= sz) k = sz - 1;
    memcpy(out, st, k); out[k] = 0;
}
static uint16_t host_hue(const char *h) {
    uint32_t x = 2166136261u;
    for (; *h; h++) { x ^= (unsigned char)*h; x *= 16777619u; }
    return (uint16_t)(x % 360);
}

static bool begin_link(Out &o, const char *spec) {
    int idx = add_link(o.pg, spec);
    if (idx < 0) return false;
    o.open = idx; o.span_a = o.len;
    return true;
}
static void ascii_put(Out &o, const char *s) { for (; *s; s++) o_ch(o, *s); }

// text between a and b with tags removed
static void feed_stripped(Out &o, const char *h, size_t a, size_t b, bool cp1251) {
    size_t i = a;
    while (i < b) {
        size_t e = i; while (e < b && h[e] != '<') e++;
        feed_text(o, h + i, e - i, cp1251);
        while (e < b && h[e] != '>') e++;
        i = e < b ? e + 1 : b;
    }
}

static bool is_void(const char *n) {
    static const char *const V[] = {"br", "hr", "img", "input", "meta", "link", "source", "wbr", "area", "col", "base", "embed", "param", "track"};
    for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++) if (!strcmp(n, V[i])) return true;
    return false;
}

// Elements that are page furniture / invisible (Wikipedia [edit] marks, navboxes, refs, cookie banners ...)
static bool skip_attrs(const char *tag, size_t tlen) {
    static const char *const CLS[] = {"mw-editsection", "navbox", "navbox-styles", "reference", "reflist", "references", "noprint",
        "mw-jump-link", "catlinks", "printfooter", "sidebar", "toc", "metadata", "sistersitebox", "mw-empty-elt", "cookie-banner",
        "visually-hidden", "sr-only", "screen-reader-text", "skip-link", "mw-cite-backlink", "vector-menu", "vector-header"};
    static char v[160];
    if (attr_get(tag, tlen, "class", v, sizeof(v))) {
        for (char *p = v; *p; ) {
            while (*p == ' ') p++;
            char *e = p; while (*e && *e != ' ') e++;
            size_t L = (size_t)(e - p);
            if (L) for (size_t k = 0; k < sizeof(CLS) / sizeof(CLS[0]); k++)
                if (strlen(CLS[k]) == L && !strncmp(CLS[k], p, L)) return true;
            p = e;
        }
    }
    if (attr_get(tag, tlen, "hidden", v, sizeof(v), true) && strcmp(v, "until-found") != 0) return true;
    if (attr_get(tag, tlen, "aria-hidden", v, sizeof(v)) && !strcmp(v, "true")) return true;
    if (attr_get(tag, tlen, "style", v, sizeof(v))) {
        if (find_ci(v, strlen(v), 0, "display:none") != (size_t)-1 || find_ci(v, strlen(v), 0, "display: none") != (size_t)-1) return true;
    }
    return false;
}

// index right after the </name> that closes the element opened just before 'from'
static size_t skip_matching(const char *h, size_t n, size_t from, const char *name) {
    size_t nl = strlen(name), i = from; int depth = 1;
    while (i < n) {
        const char *lt = (const char *)memchr(h + i, '<', n - i);
        if (!lt) return n;
        i = (size_t)(lt - h);
        if (i + 1 < n && h[i + 1] == '/' && starts_ci(h + i + 2, name) && !isalnum((unsigned char)h[i + 2 + nl])) {
            size_t e = i; while (e < n && h[e] != '>') e++;
            i = e < n ? e + 1 : n;
            if (--depth == 0) return i;
            continue;
        }
        if (starts_ci(h + i + 1, name) && !isalnum((unsigned char)h[i + 1 + nl])) depth++;
        i++;
    }
    return n;
}

static int attr_int(const char *tag, size_t tlen, const char *name) {
    char v[16];
    return attr_get(tag, tlen, name, v, sizeof(v)) ? atoi(v) : -1;
}
static bool is_svg(const char *u) { size_t L = strcspn(u, "?#"); return L >= 4 && starts_ci(u + L - 4, ".svg"); }

static void emit_image(Out &o, const char *src, const char *alt, bool tiny, bool cp1251) {
    static char full[BR_URL_MAX], spec[BR_URL_MAX + 4];
    bool ok = !tiny && src[0] && !starts_ci(src, "data:") && url_resolve(o.base, src, full, sizeof(full)) && !is_svg(full);
    if (ok && o.open >= 0 && o.len == o.span_a) o.open = -1;       // <a><img></a>: the picture itself becomes the item
    size_t al = strlen(alt);
    if (!ok || o.open >= 0) {
        if (al && !tiny) { o_ch(o, '['); feed_text(o, alt, al < 30 ? al : 30, cp1251); o_ch(o, ']'); }
        return;
    }
    snprintf(spec, sizeof(spec), "%c%c%s", 1, 'I', full);
    if (!begin_link(o, spec)) return;
    ascii_put(o, "[IMG");
    if (al) { o_ch(o, ' '); feed_text(o, alt, al < 26 ? al : 26, cp1251); }
    o_ch(o, ']');
    close_link(o);
    o_ch(o, ' ');
}

static char s_faction[BR_URL_MAX];
static char s_fhidden[240];
static char s_fname[40], s_fval[80];
static int s_flink = -1;      // link index of the text field of the current <form> (hidden fields found later are added to it)

static void form_spec(char *out, size_t sz, const char *base) {
    snprintf(out, sz, "%c%c%s%c%s%c%s%c%s", 1, 'F', s_faction[0] ? s_faction : base, 2, s_fname, 2, s_fhidden, 2, s_fval);
}

static void on_tag(Out &o, const char *name, bool closing, const char *tag, size_t tlen, bool cp1251) {
    static const char *const BLK[] = {"div", "section", "article", "header", "footer", "nav", "main", "aside",
                                      "table", "ul", "ol", "tr", "dl", "figure", "address", "fieldset",
                                      "center", "tbody", "thead", "dt", "dd", "details", "summary", "figcaption"};
    if (!strcmp(name, "a")) {
        if (!closing) {
            if (o.open < 0) {
                static char href[BR_URL_MAX], full[BR_URL_MAX];
                if (attr_get(tag, tlen, "href", href, sizeof(href)) && url_resolve(o.base, href, full, sizeof(full)))
                    begin_link(o, full);
            }
        } else close_link(o);
    } else if (!strcmp(name, "br")) {
        o_nl(o, o.nl + 1 > 2 ? 2 : o.nl + 1);
    } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        if (!closing) { o_nl(o, 2); o.cur = (name[1] == '1') ? ST_H1 : ST_H2; }
        else { o_nl(o, 1); o.cur = ST_NORM; }
    } else if (!strcmp(name, "p")) {
        o_nl(o, 2);
    } else if (!strcmp(name, "li")) {
        if (!closing) { o_nl(o, 1); o_ch(o, '*'); o_ch(o, ' '); }
    } else if (!strcmp(name, "td") || !strcmp(name, "th")) {
        if (!closing) o_ch(o, ' ');
    } else if (!strcmp(name, "hr")) {
        o_nl(o, 1); for (int i = 0; i < 8; i++) o_ch(o, '-'); o_nl(o, 1);
    } else if (!strcmp(name, "pre")) {
        o_nl(o, 1); o.pre = !closing; o.cur = closing ? ST_NORM : ST_CODE;
    } else if (!strcmp(name, "blockquote")) {
        o_nl(o, 1); o.cur = closing ? ST_NORM : ST_DIM;
    } else if (!strcmp(name, "img")) {
        if (closing) return;
        static char src[BR_URL_MAX], alt[48];
        src[0] = 0; alt[0] = 0;
        if (!attr_get(tag, tlen, "src", src, sizeof(src)) || !src[0]) attr_get(tag, tlen, "data-src", src, sizeof(src));
        attr_get(tag, tlen, "alt", alt, sizeof(alt));
        int w = attr_int(tag, tlen, "width"), hh = attr_int(tag, tlen, "height");
        emit_image(o, src, alt, (w >= 0 && w < 24) || (hh >= 0 && hh < 24), cp1251);
    } else if (!strcmp(name, "span")) {
        static char cls[100];
        if (!closing && attr_get(tag, tlen, "class", cls, sizeof(cls)) && strstr(cls, "lazy-image-placeholder")) {   // Wikipedia mobile
            static char src[BR_URL_MAX], alt[48];
            src[0] = 0; alt[0] = 0;
            attr_get(tag, tlen, "data-src", src, sizeof(src));
            attr_get(tag, tlen, "data-alt", alt, sizeof(alt));
            int w = attr_int(tag, tlen, "data-width");
            emit_image(o, src, alt, w >= 0 && w < 24, cp1251);
        }
    } else if (!strcmp(name, "form")) {
        o_nl(o, 1);
        if (!closing) {
            snprintf(s_faction, sizeof(s_faction), "%s", o.base);
            static char a[BR_URL_MAX], f2[BR_URL_MAX];
            if (attr_get(tag, tlen, "action", a, sizeof(a)) && a[0] && url_resolve(o.base, a, f2, sizeof(f2)))
                snprintf(s_faction, sizeof(s_faction), "%s", f2);
            s_fhidden[0] = 0; s_flink = -1;
        }
    } else if (!strcmp(name, "input")) {
        static char ty[16], nm[40], val[80], ph[40];
        ty[0] = 0; val[0] = 0; ph[0] = 0;
        if (attr_get(tag, tlen, "type", ty, sizeof(ty))) for (char *c = ty; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!attr_get(tag, tlen, "name", nm, sizeof(nm)) || !nm[0]) return;
        attr_get(tag, tlen, "value", val, sizeof(val));
        if (!strcmp(ty, "hidden")) {
            char e1[60], e2[120];
            url_encode(nm, e1, sizeof(e1)); url_encode(val, e2, sizeof(e2));
            size_t L = strlen(s_fhidden);
            if (L + strlen(e1) + strlen(e2) + 3 < sizeof(s_fhidden)) snprintf(s_fhidden + L, sizeof(s_fhidden) - L, "&%s=%s", e1, e2);
            if (s_flink >= 0 && s_flink < o.pg->nlinks) {          // text field came first: refresh its link
                static char spec2[BR_URL_MAX + 300];
                form_spec(spec2, sizeof(spec2), o.base);
                char *c = (char *)br_alloc(strlen(spec2) + 1);
                if (c) { memcpy(c, spec2, strlen(spec2) + 1); free(o.pg->links[s_flink]); o.pg->links[s_flink] = c; }
            }
        } else if ((!ty[0] || !strcmp(ty, "text") || !strcmp(ty, "search")) && o.open < 0) {
            attr_get(tag, tlen, "placeholder", ph, sizeof(ph));
            static char spec[BR_URL_MAX + 300];
            snprintf(s_fname, sizeof(s_fname), "%s", nm); snprintf(s_fval, sizeof(s_fval), "%s", val);
            form_spec(spec, sizeof(spec), o.base);
            if (begin_link(o, spec)) {
                s_flink = o.open;
                ascii_put(o, "[");
                const char *lab = ph[0] ? ph : (!strcmp(ty, "search") || !strcmp(nm, "q") ? "Search" : nm);
                feed_text(o, lab, strlen(lab), cp1251);
                if (val[0]) { ascii_put(o, ": "); feed_text(o, val, strlen(val), cp1251); }
                ascii_put(o, "]");
                close_link(o);
                o_ch(o, ' ');
            }
        }
    } else if (in_list(name, BLK, sizeof(BLK) / sizeof(BLK[0]))) {
        o_nl(o, 1);
    }
}

static void parse_html(Page *pg, const char *h, size_t n, bool cp1251, const char *base) {
    Out o = {}; o.buf = pg->txt; o.sty = pg->sty; o.cap = BR_TXT_CAP; o.sp = true; o.pg = pg; o.open = -1; o.base = base;
    snprintf(s_faction, sizeof(s_faction), "%s", base); s_fhidden[0] = 0;
    Out t = {}; t.buf = pg->title; t.cap = sizeof(pg->title); t.sp = true; t.open = -1;
    size_t i = 0;
    while (i < n) {
        if (h[i] == '<' && i + 1 < n && (isalpha((unsigned char)h[i + 1]) || h[i + 1] == '/' || h[i + 1] == '!' || h[i + 1] == '?')) {
            if (!strncmp(h + i, "<!--", 4)) {
                size_t e = find_ci(h, n, i + 4, "-->");
                i = (e == (size_t)-1) ? n : e + 3;
                continue;
            }
            size_t j = i + 1; char q = 0;
            while (j < n) {
                char d = h[j];
                if (q) { if (d == q) q = 0; }
                else if (d == '"' || d == '\'') q = d;
                else if (d == '>') break;
                j++;
            }
            if (j >= n) break;
            size_t p = i + 1; bool closing = false;
            if (h[p] == '/') { closing = true; p++; }
            char name[12]; int nn = 0;
            while (p < j && nn < 11 && isalnum((unsigned char)h[p])) { name[nn++] = (char)tolower((unsigned char)h[p]); p++; }
            name[nn] = 0;
            if (nn) {
                if (!closing && is_skip(name)) {
                    char pat[16]; snprintf(pat, sizeof(pat), "</%s", name);
                    size_t e = find_ci(h, n, j + 1, pat);
                    if (e == (size_t)-1) { i = n; continue; }
                    while (e < n && h[e] != '>') e++;
                    i = e < n ? e + 1 : n;
                    continue;
                }
                if (!closing && !is_void(name) && skip_attrs(h + i + 1, j - i - 1)) {
                    i = skip_matching(h, n, j + 1, name);
                    continue;
                }
                if (!closing && !strcmp(name, "title")) {
                    size_t e = find_ci(h, n, j + 1, "</title");
                    size_t te = (e == (size_t)-1) ? n : e;
                    if (!pg->title[0]) feed_text(t, h + j + 1, te - (j + 1), cp1251);
                    while (te < n && h[te] != '>') te++;
                    i = te < n ? te + 1 : n;
                    continue;
                }
                on_tag(o, name, closing, h + i + 1, j - i - 1, cp1251);
            }
            i = j + 1;
        } else {
            size_t e = i + 1;
            while (e < n && h[e] != '<') e++;
            if (e < n && !(isalpha((unsigned char)h[e + 1]) || h[e + 1] == '/' || h[e + 1] == '!' || h[e + 1] == '?') && e + 1 < n) {
                // lone '<' inside text - keep it and go on
                while (e < n && !(h[e] == '<' && e + 1 < n && (isalpha((unsigned char)h[e + 1]) || h[e + 1] == '/' || h[e + 1] == '!' || h[e + 1] == '?'))) e++;
            }
            feed_text(o, h + i, e - i, cp1251);
            i = e;
        }
    }
    close_link(o);
    pg->len = o.len;
    pg->trunc = o.trunc;
    while (t.len > 0 && pg->title[t.len - 1] == ' ') pg->title[--t.len] = 0;
}

static void parse_plain(Page *pg, const char *h, size_t n, bool cp1251) {
    Out o = {}; o.buf = pg->txt; o.sty = pg->sty; o.cap = BR_TXT_CAP; o.sp = true; o.pre = true; o.open = -1; o.pg = pg;
    feed_text(o, h, n, cp1251);
    pg->len = o.len; pg->trunc = o.trunc;
}

// DuckDuckGo lite / html results -> "1. Title" / url / snippet cards, plus a search box to refine the query
static bool parse_serp(Page *pg, const char *h, size_t n, bool cp1251, const char *base) {
    static const char *const TC[2] = {"result-link", "result__a"};
    static const char *const SC[2] = {"result-snippet", "result__snippet"};
    static const char *const UC[2] = {"link-text", "result__url"};
    int v = -1;
    for (int k = 0; k < 2 && v < 0; k++) if (find_ci(h, n, 0, TC[k]) != (size_t)-1) v = k;
    if (v < 0) return false;
    Out o = {}; o.buf = pg->txt; o.sty = pg->sty; o.cap = BR_TXT_CAP; o.sp = true; o.pg = pg; o.open = -1; o.base = base;

    static char q[100];
    q[0] = 0;
    const char *qp = strstr(base, "q=");
    if (qp) {
        qp += 2; size_t d = 0;
        while (*qp && *qp != '&' && d + 1 < sizeof(q)) {
            if (*qp == '%' && hexv(qp[1]) >= 0 && hexv(qp[2]) >= 0) { q[d++] = (char)(hexv(qp[1]) * 16 + hexv(qp[2])); qp += 3; }
            else if (*qp == '+') { q[d++] = ' '; qp++; }
            else q[d++] = *qp++;
        }
        q[d] = 0;
    }
    static char spec[BR_URL_MAX + 120];
    snprintf(spec, sizeof(spec), "%c%c%s%c%s%c%s%c%s", 1, 'F', "https://lite.duckduckgo.com/lite/", 2, "q", 2, "", 2, q);
    if (begin_link(o, spec)) {
        ascii_put(o, "[Search: "); feed_text(o, q, strlen(q), false); ascii_put(o, "]");
        close_link(o);
    }
    o_nl(o, 2);

    size_t pos = 0; int num = 0;
    while (num < 40) {
        size_t c = find_ci(h, n, pos, TC[v]);
        if (c == (size_t)-1) break;
        size_t ts = c; while (ts > 0 && h[ts] != '<') ts--;
        size_t te = c; while (te < n && h[te] != '>') te++;
        if (te >= n) break;
        size_t ae = find_ci(h, n, te + 1, "</a"); if (ae == (size_t)-1) ae = n;
        size_t nx = find_ci(h, n, ae, TC[v]);
        size_t lim = n;
        if (nx != (size_t)-1) { lim = nx; while (lim > ae && h[lim] != '<') lim--; }
        pos = ae;
        static char href[BR_URL_MAX], full[BR_URL_MAX];
        if (!attr_get(h + ts + 1, te - ts - 1, "href", href, sizeof(href)) || !url_resolve(base, href, full, sizeof(full))) continue;

        char nb[16]; snprintf(nb, sizeof(nb), "%d. ", ++num);
        ascii_put(o, nb);
        bool lk = begin_link(o, full);
        feed_stripped(o, h, te + 1, ae, cp1251);
        if (lk) close_link(o);
        o_nl(o, 1);

        size_t u = find_ci(h, n, ae, UC[v]);
        if (u != (size_t)-1 && u < lim) {
            size_t g = u; while (g < lim && h[g] != '>') g++;
            size_t e = g + 1; while (e < lim && h[e] != '<') e++;
            o.cur = ST_URL; feed_stripped(o, h, g + 1, e, cp1251); o.cur = ST_NORM;
            o_nl(o, 1);
        }
        size_t sn = find_ci(h, n, ae, SC[v]);
        if (sn != (size_t)-1 && sn < lim) {
            size_t g = sn; while (g < lim && h[g] != '>') g++;
            size_t e = find_ci(h, n, g, v == 0 ? "</td" : "</a");
            if (e == (size_t)-1 || e > lim) e = lim;
            feed_stripped(o, h, g + 1, e, cp1251);
            o_nl(o, 1);
        }
        o_nl(o, 2);
    }
    close_link(o);
    pg->len = o.len; pg->trunc = o.trunc;
    snprintf(pg->title, sizeof(pg->title), "Search: %.28s", q);
    return num > 0;
}

// ======================= layout =======================
static int layout_pass(const char *t, uint32_t n, int cols, uint32_t *off, uint16_t *len) {
    int cnt = 0; uint32_t s = 0;
    while (s <= n) {
        uint32_t e = s;
        while (e < n && t[e] != '\n') e++;
        uint32_t p = s;
        if (p == e) { if (off) { off[cnt] = p; len[cnt] = 0; } cnt++; }
        while (p < e) {
            uint32_t rem = e - p, take;
            if (rem <= (uint32_t)cols) take = rem;
            else {
                take = (uint32_t)cols;
                uint32_t k = p + cols;
                while (k > p && t[k] != ' ') k--;
                if (k > p) take = k - p;
            }
            if (off) { off[cnt] = p; len[cnt] = (uint16_t)take; }
            cnt++;
            p += take;
            while (p < e && t[p] == ' ') p++;
        }
        if (e >= n) break;
        s = e + 1;
    }
    return cnt;
}

static bool page_layout(Page *pg, int cols) {
    if (cols < 8) cols = 8;
    free(pg->loff); free(pg->llen); pg->loff = nullptr; pg->llen = nullptr;
    int n = layout_pass(pg->txt, pg->len, cols, nullptr, nullptr);
    pg->loff = (uint32_t *)br_alloc(sizeof(uint32_t) * (size_t)n);
    pg->llen = (uint16_t *)br_alloc(sizeof(uint16_t) * (size_t)n);
    if (!pg->loff || !pg->llen) return false;
    layout_pass(pg->txt, pg->len, cols, pg->loff, pg->llen);
    pg->nlines = n; pg->cols = cols;
    return true;
}

static int line_of(const Page *pg, uint32_t off) {
    int lo = 0, hi = pg->nlines - 1, r = 0;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (pg->loff[m] <= off) { r = m; lo = m + 1; } else hi = m - 1;
    }
    return r;
}

// Builds a Page from a raw HTTP body. Takes the raw buffer as is (not freed here).
static Page *page_build(const char *url, const char *raw, size_t n, const char *ctype, int cols) {
    Page *pg = (Page *)br_alloc(sizeof(Page));
    if (!pg) return nullptr;
    pg->txt = (char *)br_alloc(BR_TXT_CAP);
    pg->sty = (uint8_t *)br_alloc(BR_TXT_CAP);
    pg->spans = (Span *)br_alloc(sizeof(Span) * BR_MAX_SPANS);
    pg->links = (char **)br_alloc(sizeof(char *) * BR_MAX_LINKS);
    if (!pg->txt || !pg->sty || !pg->spans || !pg->links) { page_free(pg); return nullptr; }
    snprintf(pg->url, sizeof(pg->url), "%s", url);
    host_short(pg->url, pg->host, sizeof(pg->host));
    pg->hue = host_hue(pg->host);
    pg->cur = -1;

    bool cp1251 = find_ci(ctype, strlen(ctype), 0, "1251") != (size_t)-1;
    if (!cp1251 && find_ci(ctype, strlen(ctype), 0, "utf-8") == (size_t)-1)
        cp1251 = find_ci(raw, n < 2048 ? n : 2048, 0, "1251") != (size_t)-1;

    bool html = find_ci(ctype, strlen(ctype), 0, "html") != (size_t)-1;
    if (!html && find_ci(ctype, strlen(ctype), 0, "text/plain") == (size_t)-1 && find_ci(ctype, strlen(ctype), 0, "json") == (size_t)-1) {
        size_t m = n < 1024 ? n : 1024;
        html = find_ci(raw, m, 0, "<html") != (size_t)-1 || find_ci(raw, m, 0, "<body") != (size_t)-1 ||
               find_ci(raw, m, 0, "<!doctype html") != (size_t)-1;
    }
    bool done = false;
    if (html && (strstr(pg->url, "duckduckgo.com/lite") || strstr(pg->url, "duckduckgo.com/html")))
        done = parse_serp(pg, raw, n, cp1251, pg->url);
    if (!done) {
        for (int i = 0; i < pg->nlinks; i++) { free(pg->links[i]); pg->links[i] = nullptr; }
        pg->nlinks = 0; pg->nspans = 0; pg->len = 0; pg->txt[0] = 0; pg->title[0] = 0;
        if (html) parse_html(pg, raw, n, cp1251, pg->url);
        else parse_plain(pg, raw, n, cp1251);
    }

    while (pg->len > 0 && (pg->txt[pg->len - 1] == '\n' || pg->txt[pg->len - 1] == ' ')) pg->txt[--pg->len] = 0;
    if (pg->len == 0) { snprintf(pg->txt, BR_TXT_CAP, "(empty page)"); pg->len = (uint32_t)strlen(pg->txt); }
    if (!page_layout(pg, cols)) { page_free(pg); return nullptr; }
    return pg;
}

#ifndef K85_HOST_TEST
// ======================= network =======================
struct Fetch { char *buf; size_t len, cap; char ctype[80]; };

static esp_err_t br_http_evt(esp_http_client_event_t *e) {
    Fetch *f = (Fetch *)e->user_data;
    if (!f) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_CONNECTED) { f->len = 0; f->ctype[0] = 0; }
    else if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (e->header_key && e->header_value && starts_ci(e->header_key, "content-type") && strlen(e->header_key) == 12)
            snprintf(f->ctype, sizeof(f->ctype), "%s", e->header_value);
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
        size_t room = f->cap - f->len;
        size_t k = (size_t)e->data_len < room ? (size_t)e->data_len : room;
        if (k) { memcpy(f->buf + f->len, e->data, k); f->len += k; }
    }
    return ESP_OK;
}

static int s_cols = 21, s_rows = 11;
#define BR_LH 10

static void draw_status(const char *l1, const char *l2) {
    auto &d = M5.Display;
    uint32_t bg = k85_get_bg(), fg = k85_get_fg();
    d.fillScreen(bg);
    d.setTextSize(1); d.setTextWrap(false); d.setTextColor(fg, bg);
    int w = d.width(), h = d.height();
    int x1 = (w - (int)strlen(l1) * 6) / 2; if (x1 < 2) x1 = 2;
    d.setCursor(x1, h / 2 - 10); d.print(l1);
    if (l2 && l2[0]) {
        int x2 = (w - (int)strlen(l2) * 6) / 2; if (x2 < 2) x2 = 2;
        d.setCursor(x2, h / 2 + 4); d.print(l2);
    }
}

static void wait_ack(uint32_t ms) {
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) < (int64_t)ms * 1000) {
        k85_input_update();
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
static void wait_release(void) {
    for (int i = 0; i < 100; i++) {
        k85_input_update();
        if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool ensure_wifi(char *err, size_t errsz) {
    if (k85_wifi_is_connected()) return true;
    draw_status("Connecting WiFi...", "");
    if (k85_wifi_connect_saved()) return true;
    snprintf(err, errsz, "No WiFi");
    return false;
}

// GET url into f.buf (caller frees it). fin = final URL after redirects.
static bool http_get(const char *url, size_t cap, bool range, Fetch &f, char *fin, size_t finsz, char *err, size_t errsz) {
    f = Fetch{};
    fin[0] = 0;
    if (!ensure_wifi(err, errsz)) return false;
    K85HeavyLockGuard lock(20000);
    if (!lock.held) { snprintf(err, errsz, "Busy, retry"); return false; }
    f.cap = cap;
    f.buf = (char *)br_alloc(cap + 1);
    if (!f.buf) { snprintf(err, errsz, "Out of memory"); return false; }

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = br_http_evt;
    cfg.user_data = &f;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 12000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    cfg.max_redirection_count = 6;
    cfg.user_agent = "Mozilla/5.0 (k85OS; ESP32-S3) TextBrowser";
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) { free(f.buf); f.buf = nullptr; snprintf(err, errsz, "HTTP init failed"); return false; }
    esp_http_client_set_header(cl, "Accept", range ? "text/html,text/plain;q=0.9,*/*;q=0.5" : "image/jpeg,image/png,*/*;q=0.5");
    esp_http_client_set_header(cl, "Accept-Language", "en,ru;q=0.8,cs;q=0.6");
    if (range) {
        char rg[40]; snprintf(rg, sizeof(rg), "bytes=0-%u", (unsigned)(cap - 1));      // big pages: only the first part
        esp_http_client_set_header(cl, "Range", rg);
    }
    esp_err_t r = esp_http_client_perform(cl);
    int status = esp_http_client_get_status_code(cl);
    esp_http_client_get_url(cl, fin, (int)finsz);
    esp_http_client_cleanup(cl);

    if (r != ESP_OK) snprintf(err, errsz, "%.24s", esp_err_to_name(r));
    else if (status != 200 && status != 206) snprintf(err, errsz, "HTTP %d", status);
    else {
        for (char *c = f.ctype; *c; c++) *c = (char)tolower((unsigned char)*c);
        f.buf[f.len] = 0;
        return true;
    }
    free(f.buf); f.buf = nullptr;
    return false;
}

static void host_line(const char *url, char *out, size_t sz) {
    const char *h = strstr(url, "://");
    snprintf(out, sz, "%.26s", h ? h + 3 : url);
}

// Returns a new page or nullptr (err filled with a short reason).
// K85JV1: a local file as a page (file:///littlefs/x.html or file:///sd/x.html)
static Page *fetch_local(const char *url, char *err, size_t errsz) {
    draw_status("Opening file...", "");
    static char path[BR_URL_MAX];
    const char *p = url + 7;
    const size_t pl = strcspn(p, "?#");
    if (pl == 0 || pl >= sizeof(path)) { snprintf(err, errsz, "Bad path"); return nullptr; }
    size_t o = 0;
    for (size_t i = 0; i < pl && o + 1 < sizeof(path); i++) {
        if (p[i] == '%' && i + 2 < pl && hexv(p[i + 1]) >= 0 && hexv(p[i + 2]) >= 0) { path[o++] = (char)(hexv(p[i + 1]) * 16 + hexv(p[i + 2])); i += 2; }
        else path[o++] = p[i];
    }
    path[o] = 0;
    if (strstr(path, "..") || (strncmp(path, "/littlefs/", 10) != 0 && strncmp(path, "/sd/", 4) != 0) || k85_is_blocked_path(path)) {
        snprintf(err, errsz, "Not allowed");
        return nullptr;
    }
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) { snprintf(err, errsz, "No such file"); return nullptr; }
    char *buf = (char *)br_alloc(BR_RAW_MAX + 1);
    if (!buf) { snprintf(err, errsz, "Out of memory"); return nullptr; }
    FILE *f = fopen(path, "rb");
    if (!f) { free(buf); snprintf(err, errsz, "Cannot open"); return nullptr; }
    const size_t n = fread(buf, 1, BR_RAW_MAX, f);
    fclose(f);
    buf[n] = 0;
    const char *dot = strrchr(path, '.');
    const bool html = dot && (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm") || !strcasecmp(dot, ".xhtml"));
    Page *pg = page_build(url, buf, n, html ? "text/html" : "text/plain; charset=utf-8", s_cols);
    free(buf);
    if (!pg) { snprintf(err, errsz, "Out of memory"); return nullptr; }
    if ((size_t)st.st_size > n) pg->trunc = true;
    snprintf(pg->host, sizeof(pg->host), "local");
    return pg;
}

// Returns a new page or nullptr (err filled with a short reason).
static Page *fetch_page(const char *url, char *err, size_t errsz) {
    if (starts_ci(url, "file://")) return fetch_local(url, err, errsz);  // K85JV1
    char l2[28]; host_line(url, l2, sizeof(l2));
    draw_status("Loading...", l2);
    static char fin[BR_URL_MAX];
    Fetch f;
    if (!http_get(url, BR_RAW_MAX, true, f, fin, sizeof(fin), err, errsz)) return nullptr;
    if (f.ctype[0] && !strstr(f.ctype, "text/") && !strstr(f.ctype, "html") && !strstr(f.ctype, "xml") && !strstr(f.ctype, "json")) {
        snprintf(err, errsz, "Unsupported:%.14s", f.ctype); free(f.buf); return nullptr;
    }
    Page *pg = page_build(fin[0] ? fin : url, f.buf, f.len, f.ctype, s_cols);
    if (pg && f.len >= f.cap) pg->trunc = true;
    free(f.buf);
    if (!pg) snprintf(err, errsz, "Out of memory");
    return pg;
}

// ======================= images =======================
static bool img_dims(const uint8_t *b, size_t n, int *type, int *w, int *h, bool *prog) {
    *prog = false;
    if (n > 24 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        *type = 2;
        *w = (int)(((uint32_t)b[16] << 24) | ((uint32_t)b[17] << 16) | ((uint32_t)b[18] << 8) | b[19]);
        *h = (int)(((uint32_t)b[20] << 24) | ((uint32_t)b[21] << 16) | ((uint32_t)b[22] << 8) | b[23]);
        return *w > 0 && *h > 0;
    }
    if (n > 4 && b[0] == 0xFF && b[1] == 0xD8) {
        *type = 1;
        size_t p = 2;
        while (p + 9 < n) {
            if (b[p] != 0xFF) { p++; continue; }
            uint8_t m = b[p + 1];
            if (m == 0xFF) { p++; continue; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { p += 2; continue; }
            int len = (b[p + 2] << 8) | b[p + 3];
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                *h = (b[p + 5] << 8) | b[p + 6];
                *w = (b[p + 7] << 8) | b[p + 8];
                *prog = (m != 0xC0 && m != 0xC1);
                return *w > 0 && *h > 0;
            }
            if (len < 2) break;
            p += 2 + len;
        }
    }
    return false;
}

// Full-screen image viewer (JPEG baseline / PNG), scaled to fit. Any button closes it.
static void show_image(const char *url) {
    static char fin[BR_URL_MAX], err[40];
    Fetch f;
    char l2[28]; host_line(url, l2, sizeof(l2));
    draw_status("Loading image...", l2);
    if (!http_get(url, BR_IMG_MAX, false, f, fin, sizeof(fin), err, sizeof(err))) { draw_status("Image failed", err); wait_ack(2500); return; }
    int type = 0, w = 0, h = 0; bool prog = false;
    const char *msg = nullptr;
    if (f.len >= f.cap) msg = "Image too big";
    else if (!img_dims((const uint8_t *)f.buf, f.len, &type, &w, &h, &prog)) msg = "Not a JPG/PNG";
    else if (prog) msg = "Progressive JPG n/a";
    if (msg) { free(f.buf); draw_status(msg, l2); wait_ack(2500); return; }

    auto &d = M5.Display;
    int W = d.width(), H = d.height();
    float sc = (float)W / (float)w, sy = (float)H / (float)h;
    if (sy < sc) sc = sy;
    if (sc > 1.0f) sc = 1.0f;
    int ox = (W - (int)((float)w * sc)) / 2, oy = (H - (int)((float)h * sc)) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    d.fillScreen(0x000000);
    bool ok = (type == 1) ? d.drawJpg((const uint8_t *)f.buf, (uint32_t)f.len, ox, oy, 0, 0, 0, 0, sc, sc)
                          : d.drawPng((const uint8_t *)f.buf, (uint32_t)f.len, ox, oy, 0, 0, 0, 0, sc, sc);
    free(f.buf);
    if (!ok) { draw_status("Cannot decode image", ""); wait_ack(2500); return; }
    wait_ack(120000);
    wait_release();
}

// ======================= UI =======================
static uint32_t mixc(uint32_t a, uint32_t b, int t) {     // a -> b, t = 0..255
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255, br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return ((uint32_t)(ar + (br - ar) * t / 255) << 16) | ((uint32_t)(ag + (bg - ag) * t / 255) << 8) | (uint32_t)(ab + (bb - ab) * t / 255);
}
static int lumi(uint32_t c) { return (int)((((c >> 16) & 255) * 30 + ((c >> 8) & 255) * 59 + (c & 255) * 11) / 100); }
static uint32_t hsv(int h, int s, int v) {
    int sec = h / 60, f = (h % 60) * 255 / 60;
    int p = v * (255 - s) / 255, q = v * (255 - s * f / 255) / 255, t = v * (255 - s * (255 - f) / 255) / 255, r, g, b;
    switch (sec) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void draw_page(const Page *pg) {
    auto &d = M5.Display;
    uint32_t bg = k85_get_bg(), fg = k85_get_fg(), ac = k85_get_accent();
    bool light = lumi(bg) > 128;
    uint32_t site = hsv(pg->hue, light ? 200 : 150, light ? 150 : 235);     // every site gets its own colour
    uint32_t onsite = lumi(site) > 140 ? 0x000000 : 0xFFFFFF;
    uint32_t dim = mixc(fg, bg, 120), urlc = light ? 0x007A33 : 0x4CD97B, tint = mixc(bg, fg, 28);
    int w = d.width(), h = d.height();
    d.fillScreen(bg);
    d.setTextSize(1); d.setTextWrap(false);

    // header: site name on the site colour, then the page title
    d.fillRect(0, 0, w, BR_LH, site);
    d.setTextColor(onsite, site);
    char hb[96];
    if (pg->title[0]) snprintf(hb, sizeof(hb), "%s | %s", pg->host, pg->title); else snprintf(hb, sizeof(hb), "%s", pg->host);
    if ((int)strlen(hb) > s_cols) hb[s_cols] = 0;
    d.setCursor(2, 1); d.print(hb);

    // body
    for (int r = 0; r < s_rows; r++) {
        int li = pg->top + r;
        if (li >= pg->nlines) break;
        int y = BR_LH + 1 + r * BR_LH;
        uint32_t pos = pg->loff[li], le = pos + pg->llen[li];
        if (pos < le && pg->sty[pos] == ST_H1) d.fillRect(0, y - 1, w, BR_LH, site);
        int x = 2;
        while (pos < le) {
            int si = -1; uint32_t end = le;
            for (int s = 0; s < pg->nspans; s++) {
                const Span &sp = pg->spans[s];
                if (sp.b <= pos) continue;
                if (sp.a <= pos) { si = s; end = sp.b < le ? sp.b : le; }
                else end = sp.a < le ? sp.a : le;
                break;
            }
            uint8_t st = pg->sty[pos];
            for (uint32_t k = pos + 1; k < end; k++) if (pg->sty[k] != st) { end = k; break; }
            char seg[96]; uint32_t L = end - pos; if (L > sizeof(seg) - 1) L = sizeof(seg) - 1;
            memcpy(seg, pg->txt + pos, L); seg[L] = 0;
            int sw = (int)L * 6;
            uint32_t cf = fg, cb = bg; bool ul = false;
            if (st == ST_H1) { cf = onsite; cb = site; }
            else if (st == ST_H2) cf = site;
            else if (st == ST_DIM) cf = dim;
            else if (st == ST_URL) cf = urlc;
            else if (st == ST_CODE) cf = mixc(fg, bg, 60);
            if (si >= 0) {
                bool sel = (si == pg->cur);
                int kind = link_kind(pg->links[pg->spans[si].link]);
                if (sel) { cf = 0x000000; cb = ac; }
                else if (kind == LK_URL) { cf = ac; ul = true; }
                else { cf = ac; cb = tint; }              // pictures and input fields look like buttons
            }
            d.setTextColor(cf, cb);
            d.setCursor(x, y); d.print(seg);
            if (ul) d.drawFastHLine(x, y + BR_LH - 2, sw, cf);
            x += sw; pos += L ? L : 1;
        }
    }

    // footer: target of the selected item, or position
    char fb[96];
    bool sel = pg->cur >= 0 && pg->cur < pg->nspans;
    if (sel) {
        const char *l = pg->links[pg->spans[pg->cur].link];
        int kind = link_kind(l);
        const char *u = strstr(kind == LK_URL ? l : l + 2, "://");
        if (kind == LK_URL) snprintf(fb, sizeof(fb), "> %s", u ? u + 3 : l);
        else if (kind == LK_IMG) snprintf(fb, sizeof(fb), "img %s", u ? u + 3 : l + 2);
        else snprintf(fb, sizeof(fb), "B = type text");
    } else {
        int bottom = pg->top + s_rows > pg->nlines ? pg->nlines : pg->top + s_rows;
        int pct = pg->nlines ? (int)(((int64_t)bottom * 100) / pg->nlines) : 100;
        snprintf(fb, sizeof(fb), "%d%% %d items%s%s", pct, pg->nspans, pg->trunc ? " cut" : "", s_cols >= 30 ? " " BR_HINT : "");
    }
    if ((int)strlen(fb) > s_cols) fb[s_cols] = 0;
    d.setTextColor(sel ? ac : dim, bg);
    d.setCursor(2, h - BR_LH + 1); d.print(fb);
}

static void scroll_to_show(Page *pg, int li) {
    if (li < pg->top || li >= pg->top + s_rows) {
        pg->top = li - 1;
        if (pg->top < 0) pg->top = 0;
    }
    int mx = pg->nlines - s_rows; if (mx < 0) mx = 0;
    if (pg->top > mx) pg->top = mx;
}

static void nav_next(Page *pg) {
    int mx = pg->nlines - s_rows; if (mx < 0) mx = 0;
    if (pg->nspans > 0 && pg->cur < pg->nspans - 1) {
        if (pg->cur < 0) {                    // nothing selected yet: first item on the current screen
            int s = 0;
            while (s < pg->nspans - 1 && line_of(pg, pg->spans[s].a) < pg->top) s++;
            pg->cur = s;
        } else pg->cur++;
        scroll_to_show(pg, line_of(pg, pg->spans[pg->cur].a));
    } else if (pg->top < mx) {
        pg->top += s_rows - 1; if (pg->top > mx) pg->top = mx;
    } else { pg->top = 0; pg->cur = -1; }
}

static void nav_prev(Page *pg) {
    if (pg->nspans > 0 && pg->cur > 0) {
        pg->cur--;
        scroll_to_show(pg, line_of(pg, pg->spans[pg->cur].a));
    } else if (pg->top > 0) {
        pg->top -= s_rows - 1; if (pg->top < 0) pg->top = 0;
        if (pg->cur > 0) pg->cur = -1;
    }
}

// ----- bookmarks -----
static int bm_load(char bm[][BR_URL_MAX]) {
    FILE *f = fopen(BR_BM_FILE, "r");
    if (!f) return 0;
    int n = 0; char line[BR_URL_MAX];
    while (n < BR_BM_MAX && fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = 0;
        if (L > 8) snprintf(bm[n++], BR_URL_MAX, "%s", line);
    }
    fclose(f);
    return n;
}
static void bm_save(char bm[][BR_URL_MAX], int n) {
    FILE *f = fopen(BR_BM_FILE, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", bm[i]);
    fclose(f);
}

static void keep_alive_input(void) { k85_wake_screen(); }

static void make_url(const char *in, char *out, size_t sz) {
    while (*in == ' ') in++;
    if (in[0] == '/') { snprintf(out, sz, "file://%s", in); return; }  // K85JV1: a local path
    if (strstr(in, "://")) { snprintf(out, sz, "%s", in); return; }
    if (strchr(in, ' ') || !strchr(in, '.')) {
        char enc[160]; url_encode(in, enc, sizeof(enc));
        snprintf(out, sz, "https://lite.duckduckgo.com/lite/?q=%s", enc);
        return;
    }
    snprintf(out, sz, "https://%s", in);
}

static void ask_url(char *out, size_t sz) {
    static char in[BR_URL_MAX - 20];
    in[0] = 0; out[0] = 0;
    if (!k85_text_input("URL or search:", "", in, sizeof(in)) || !in[0]) return;
    make_url(in, out, sz);
}

// keyboard -> search URL. fmt gets the url-encoded query ("%s").
static void ask_search(const char *prompt, const char *fmt, char *out, size_t sz) {
    static char in[100];
    in[0] = 0; out[0] = 0;
    if (!k85_text_input(prompt, "", in, sizeof(in)) || !in[0]) return;
    char enc[200]; url_encode(in, enc, sizeof(enc));
    snprintf(out, sz, fmt, enc);
}

// "form link" -> keyboard -> GET url
static bool form_url(const char *spec, char *out, size_t sz) {
    static char buf[BR_URL_MAX + 300];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *action = buf, *name = strchr(action, 2);
    if (!name) return false;
    *name++ = 0;
    char *hid = strchr(name, 2);
    if (!hid) return false;
    *hid++ = 0;
    char *val = strchr(hid, 2);
    if (!val) return false;
    *val++ = 0;
    static char in[100];
    in[0] = 0;
    if (!k85_text_input("Search:", val, in, sizeof(in)) || !in[0]) return false;
    char enc[200]; url_encode(in, enc, sizeof(enc));
    snprintf(out, sz, "%s%c%s=%s%s", action, strchr(action, '?') ? '&' : '?', name, enc, hid);
    return true;
}

// Page viewer. Returns when the user leaves the browser page (A+B with empty history or menu -> Exit).
static void browse(const char *start_url) {
    static char (*hist)[BR_URL_MAX] = nullptr;
    int hn = 0;
    if (!hist) hist = (char (*)[BR_URL_MAX])br_alloc((size_t)BR_HIST_MAX * BR_URL_MAX);
    if (!hist) { k85_show_message("Out of memory"); return; }

    Page *pg = nullptr;
    char err[40];
    static char pending[BR_URL_MAX * 2 + 100];
    snprintf(pending, sizeof(pending), "%s", start_url);
    bool dirty = true, chord = false;
    int64_t a_rep = 0;

    auto load = [&](const char *url, bool push) -> void {
        Page *np = fetch_page(url, err, sizeof(err));
        if (!np) {
            draw_status("Load failed", err);
            wait_ack(3000);
            return;
        }
        if (push && pg && hn < BR_HIST_MAX) snprintf(hist[hn++], BR_URL_MAX, "%s", pg->url);
        else if (push && pg) { memmove(hist[0], hist[1], (size_t)(BR_HIST_MAX - 1) * BR_URL_MAX); snprintf(hist[BR_HIST_MAX - 1], BR_URL_MAX, "%s", pg->url); }
        page_free(pg);
        pg = np;
        k85_log("Browser: %.40s", pg->url);
    };

    load(pending, false);
    if (!pg) { return; }

    while (true) {
        if (dirty) { draw_page(pg); dirty = false; }
        k85_input_update();

        if (k85_ab_held(500)) {
            k85_wait_ab_release(); chord = false;
            if (hn > 0) {
                static char back[BR_URL_MAX]; snprintf(back, sizeof(back), "%s", hist[--hn]);
                load(back, false); dirty = true;
                continue;
            }
            break;
        }
        if (k85_check_ab_hold()) chord = true;
        if (chord) {
            if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) chord = false;
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        bool a_click = M5.BtnA.wasClicked(), b_click = M5.BtnB.wasClicked(), b_hold = M5.BtnB.wasHold();
        if (M5.BtnA.isHolding() && !M5.BtnB.isPressed()) {
            int64_t now = esp_timer_get_time();
            if (now - a_rep >= 200000) { a_rep = now; keep_alive_input(); nav_prev(pg); dirty = true; }
        }
        if (a_click) { keep_alive_input(); nav_next(pg); dirty = true; }

        if (b_click) {
            keep_alive_input();
            if (pg->cur >= 0 && pg->cur < pg->nspans) {
                const char *lk = pg->links[pg->spans[pg->cur].link];
                int kind = link_kind(lk);
                if (kind == LK_IMG) {
                    snprintf(pending, sizeof(pending), "%s", lk + 2);
                    show_image(pending);
                    wait_release();
                } else if (kind == LK_FORM) {
                    if (form_url(lk + 2, pending, sizeof(pending))) load(pending, true);
                } else {
                    snprintf(pending, sizeof(pending), "%s", lk);
                    load(pending, true);
                }
                dirty = true;
            } else { nav_next(pg); dirty = true; }
        }

        if (b_hold) {
            keep_alive_input();
            static const char *const items[] = {"Search web", "Go to URL", "Back", "Reload", "Bookmark page", "Bookmarks", "Exit browser", "Close menu"};
            static const char *const keys[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
            int sel = k85_run_list_menu("Browser", items, 8, keys);
            if (sel == 0) {
                ask_search("Search:", "https://lite.duckduckgo.com/lite/?q=%s", pending, sizeof(pending));
                if (pending[0]) load(pending, true);
            } else if (sel == 1) {
                ask_url(pending, sizeof(pending));
                if (pending[0]) load(pending, true);
            } else if (sel == 2) {
                if (hn > 0) { static char back[BR_URL_MAX]; snprintf(back, sizeof(back), "%s", hist[--hn]); load(back, false); }
            } else if (sel == 3) {
                snprintf(pending, sizeof(pending), "%s", pg->url); load(pending, false);
            } else if (sel == 4) {
                static EXT_RAM_BSS_ATTR char bm[BR_BM_MAX][BR_URL_MAX];
                int n = bm_load(bm);
                bool have = false;
                for (int i = 0; i < n; i++) if (!strcmp(bm[i], pg->url)) have = true;
                if (!have && n < BR_BM_MAX) { snprintf(bm[n++], BR_URL_MAX, "%s", pg->url); bm_save(bm, n); draw_status("Bookmarked", ""); }
                else draw_status(have ? "Already saved" : "Bookmarks full", "");
                wait_ack(1200);
            } else if (sel == 5) {
                static EXT_RAM_BSS_ATTR char bm[BR_BM_MAX][BR_URL_MAX];
                int n = bm_load(bm);
                if (n == 0) { draw_status("No bookmarks", ""); wait_ack(1200); }
                else {
                    static char lab[BR_BM_MAX + 2][40];
                    const char *li[BR_BM_MAX + 2]; const char *lk[BR_BM_MAX + 2];
                    for (int i = 0; i < n; i++) {
                        const char *u = strstr(bm[i], "://"); u = u ? u + 3 : bm[i];
                        snprintf(lab[i], sizeof(lab[i]), "%.38s", u); li[i] = lab[i]; lk[i] = nullptr;
                    }
                    snprintf(lab[n], sizeof(lab[n]), "Clear all"); li[n] = lab[n]; lk[n] = nullptr;
                    int s2 = k85_run_list_menu("Bookmarks", li, n + 1, lk);
                    if (s2 >= 0 && s2 < n) { snprintf(pending, sizeof(pending), "%s", bm[s2]); load(pending, true); }
                    else if (s2 == n) { bm_save(bm, 0); draw_status("Cleared", ""); wait_ack(1000); }
                }
            } else if (sel == 6) break;
            dirty = true;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    page_free(pg);
}

// K85JV1: open a URL or a file:// page directly (used by the file manager)
void k85_browser_open_url(const char *url) {
    auto &d = M5.Display;
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    s_cols = (d.width() - 4) / 6;
    s_rows = (d.height() - 2 * BR_LH - 2) / BR_LH;
    if (s_rows < 4) s_rows = 4;
    static char u[BR_URL_MAX * 2 + 100];
    snprintf(u, sizeof(u), "%s", url);
    browse(u);
    k85_apply_font(g_config.font_idx);
}

void k85_run_browser(void) {
    auto &d = M5.Display;
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    s_cols = (d.width() - 4) / 6;
    s_rows = (d.height() - 2 * BR_LH - 2) / BR_LH;
    if (s_rows < 4) s_rows = 4;

    static const char *const items[] = {"Search web", "Go to URL", "Bookmarks", "Wikipedia EN", "Wikipedia RU", "Hacker News", "Text news (NPR)", "Back"};
    static const char *const keys[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    while (true) {
        int sel = k85_run_list_menu("Browser", items, 8, keys);
        if (sel < 0 || sel == 7) break;
        static char url[BR_URL_MAX * 2 + 100];
        url[0] = 0;
        if (sel == 0) ask_search("Search:", "https://lite.duckduckgo.com/lite/?q=%s", url, sizeof(url));
        else if (sel == 1) ask_url(url, sizeof(url));
        else if (sel == 2) {
            static EXT_RAM_BSS_ATTR char bm[BR_BM_MAX][BR_URL_MAX];
            int n = bm_load(bm);
            if (n == 0) { draw_status("No bookmarks yet", "hold B in a page"); wait_ack(2000); continue; }
            static char lab[BR_BM_MAX][40];
            const char *li[BR_BM_MAX]; const char *lk[BR_BM_MAX];
            for (int i = 0; i < n; i++) {
                const char *u = strstr(bm[i], "://"); u = u ? u + 3 : bm[i];
                snprintf(lab[i], sizeof(lab[i]), "%.38s", u); li[i] = lab[i]; lk[i] = nullptr;
            }
            int s2 = k85_run_list_menu("Bookmarks", li, n, lk);
            if (s2 >= 0 && s2 < n) snprintf(url, sizeof(url), "%s", bm[s2]);
        } else if (sel == 3) ask_search("Wikipedia EN:", "https://en.m.wikipedia.org/wiki/Special:Search?search=%s&go=Go", url, sizeof(url));
        else if (sel == 4) ask_search("Wikipedia RU:", "https://ru.m.wikipedia.org/wiki/Special:Search?search=%s&go=Go", url, sizeof(url));
        else if (sel == 5) snprintf(url, sizeof(url), "https://news.ycombinator.com/");
        else if (sel == 6) snprintf(url, sizeof(url), "https://text.npr.org/");
        if (url[0]) browse(url);
    }
    k85_apply_font(g_config.font_idx);
}
#endif  // !K85_HOST_TEST