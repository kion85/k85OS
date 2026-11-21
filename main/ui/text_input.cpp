// K85KBD2: full on-screen keyboard: digits, QWERTY / Russian, Shift, CAPS, a Fn layer with all the symbols,
// an Edt layer (select, copy, cut, paste, clipboard history, Home / End / Up / Down), a real space bar,
// Enter, Del, language switch and a text cursor that moves with < > (editing in the middle of the text).
// One engine for the single-line and the multi-line editor.
// A = next key, hold A = next row, B = press, A+B (hold) = accept; a tap from the web page presses a key directly.
#include "text_input.h"
#include "esp_attr.h"
#include "theme.h"
#include "battery.h"
#include "status_bar.h"
#include "power.h"
#include "input.h"
#include "config.h"
#include "clipboard.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cmath>
#include "esp_timer.h"

#define K85_TI_KB_IDLE_MS 5000
#define TI_MAXK 14
#define TI_LMAX 2100

static int64_t k85_ti_ticks_ms() { return esp_timer_get_time() / 1000; }

static EXT_RAM_BSS_ATTR uint16_t s_lstart[TI_LMAX];
static int s_nl = 0;

static int ti_u8len(const char *s) {
    int n = 0;
    for (; *s; s++) if ((((unsigned char)*s) & 0xC0) != 0x80) n++;
    return n;
}
static inline int ti_cplen(unsigned char c) { return c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1)); }
static int ti_prev(const char *t, int pos) {
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && (((unsigned char)t[p]) & 0xC0) == 0x80) p--;
    return p;
}
static int ti_next(const char *t, int pos) {
    if (!t[pos]) return pos;
    const int len = (int)strlen(t);
    const int n = pos + ti_cplen((unsigned char)t[pos]);
    return n > len ? len : n;
}
static int ti_cpcount(const char *t, int from, int to) {
    int n = 0;
    for (int i = from; i < to; ) {
        if (!t[i]) break;
        i = ti_next(t, i);
        n++;
    }
    return n;
}
static int ti_byte_at_cp(const char *t, int from, int cps, int limit) {
    int i = from;
    while (cps > 0 && i < limit && t[i]) { i = ti_next(t, i); cps--; }
    return i;
}

// lower case -> upper case for an ASCII letter or a 2-byte UTF-8 Russian letter
static void ti_upper(char *b) {
    const unsigned char a = (unsigned char)b[0], c = (unsigned char)b[1];
    if (a < 0x80) { b[0] = (char)toupper(a); return; }
    if (a == 0xD0 && c >= 0xB0 && c <= 0xBF) b[1] = (char)(c - 0x20);
    else if (a == 0xD1 && c >= 0x80 && c <= 0x8F) { b[0] = (char)0xD0; b[1] = (char)(c + 0x20); }
    else if (a == 0xD1 && c == 0x91) { b[0] = (char)0xD0; b[1] = (char)0x81; }
}

static void ti_wipe(char *p, size_t n) {
    volatile char *v = p;
    while (n--) *v++ = 0;
}

static void ti_first_line(const char *s, char *dst, size_t dsz, int maxcp) {
    size_t o = 0;
    int cp = 0;
    while (*s && *s != '\n' && *s != '\r') {
        if (cp >= maxcp) {
            if (o + 2 < dsz) { dst[o++] = '.'; dst[o++] = '.'; }
            break;
        }
        const size_t l = (size_t)ti_cplen((unsigned char)*s);
        if (strnlen(s, l) < l) break;
        if (o + l + 1 > dsz) break;
        memcpy(dst + o, s, l);
        o += l; s += l; cp++;
    }
    dst[o] = 0;
}

// insert s at *cur (cut at a character boundary if it does not fit)
static bool ti_ins(char *t, size_t cap, int *cur, const char *s) {
    const size_t len = strlen(t);
    size_t n = strlen(s);
    if (len + 1 >= cap) return false;
    const size_t room = cap - 1 - len;
    if (n > room) {
        n = room;
        while (n > 0 && (((unsigned char)s[n]) & 0xC0) == 0x80) n--;
    }
    if (n == 0) return false;
    memmove(t + *cur + n, t + *cur, len - (size_t)*cur + 1);
    memcpy(t + *cur, s, n);
    *cur += (int)n;
    return true;
}
static void ti_del(char *t, int a, int b) {
    if (b <= a) return;
    memmove(t + a, t + b, strlen(t + b) + 1);
}

// ---- multi-line layout: display lines (wrapped at `cols`, split at '\n') ----
static void ti_layout(const char *t, int cols) {
    s_nl = 0;
    s_lstart[s_nl++] = 0;
    int col = 0;
    for (int i = 0; t[i] && s_nl < TI_LMAX - 1; ) {
        if (t[i] == '\n') { i++; s_lstart[s_nl++] = (uint16_t)i; col = 0; continue; }
        if (col >= cols) { s_lstart[s_nl++] = (uint16_t)i; col = 0; }
        i = ti_next(t, i);
        col++;
    }
}
static int ti_line_of(int pos) {
    int l = 0;
    for (int i = 0; i < s_nl; i++) {
        if (s_lstart[i] <= pos) l = i; else break;
    }
    return l;
}
static int ti_line_end(const char *t, int l) {
    int e = (l + 1 < s_nl) ? (int)s_lstart[l + 1] : (int)strlen(t);
    if (e > (int)s_lstart[l] && t[e - 1] == '\n') e--;
    return e;
}
static int ti_vmove(const char *t, int cur, int cols, int dir) {
    ti_layout(t, cols);
    const int l = ti_line_of(cur);
    int col = ti_cpcount(t, s_lstart[l], cur);
    const int tl = l + dir;
    if (tl < 0 || tl >= s_nl) return cur;
    const int e = ti_line_end(t, tl);
    const int lcp = ti_cpcount(t, s_lstart[tl], e);
    int maxc = lcp;
    if (tl + 1 < s_nl && e == (int)s_lstart[tl + 1]) maxc = lcp > 0 ? lcp - 1 : 0;
    if (col > maxc) col = maxc;
    return ti_byte_at_cp(t, s_lstart[tl], col, e);
}
static int ti_home(const char *t, int cur) {
    int i = cur;
    while (i > 0 && t[i - 1] != '\n') i--;
    return i;
}
static int ti_end(const char *t, int cur) {
    int i = cur;
    while (t[i] && t[i] != '\n') i++;
    return i;
}

// ---- keyboard layers ----
struct TiKey { char t[8]; uint8_t w; };
struct TiRow { TiKey k[TI_MAXK]; int n; int total; };
struct TiKb { TiRow r[5]; };

#define TI_BOT "FN EDIT LANG SPACE , . LEFT RIGHT OK EXIT"
static const char *const TI_EN[5] = {
    "1 2 3 4 5 6 7 8 9 0 DEL",
    "q w e r t y u i o p",
    "CAPS a s d f g h j k l ENTER",
    "SHIFT z x c v b n m - _",
    TI_BOT,
};
static const char *const TI_RU[5] = {
    "1 2 3 4 5 6 7 8 9 0 DEL",
    "й ц у к е н г ш щ з х ъ",
    "CAPS ф ы в а п р о л д ж э ENTER",
    "SHIFT я ч с м и т ь б ю ё",
    TI_BOT,
};
static const char *const TI_SYM[5] = {
    "1 2 3 4 5 6 7 8 9 0 DEL",
    "! @ # $ % ^ & * ( )",
    "` ~ - _ = + [ ] { }",
    "\\ | ; : ' \" < > ? /",
    TI_BOT,
};
static const char *const TI_EDIT[5] = {
    "MARK ALL COPY CUT PASTE CLIP",
    "HOME END UP DOWN DEL ENTER",
    "TAB CLEAR SPACE",
    ", . ? ! : ; ' \"",
    TI_BOT,
};

static int ti_w(const char *t) {
    if ((unsigned char)t[0] >= 0x80) return 2;
    if (!strcmp(t, "SPACE")) return 7;
    if (t[1] == 0) return 2;
    if (!strcmp(t, "LEFT") || !strcmp(t, "RIGHT")) return 2;
    if (!strcmp(t, "FN") || !strcmp(t, "LANG") || !strcmp(t, "OK") || !strcmp(t, "UP") || !strcmp(t, "ALL")) return 3;
    return 4;
}

static void ti_build(TiKb &kb, const char *const rows[5]) {
    for (int r = 0; r < 5; r++) {
        TiRow &R = kb.r[r];
        R.n = 0;
        R.total = 0;
        const char *p = rows[r];
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *e = p;
            while (*e && *e != ' ') e++;
            size_t l = (size_t)(e - p);
            if (l > 7) l = 7;
            if (R.n < TI_MAXK) {
                TiKey &K = R.k[R.n++];
                memcpy(K.t, p, l);
                K.t[l] = 0;
                K.w = (uint8_t)ti_w(K.t);
                R.total += K.w;
            }
            p = e;
        }
    }
}

static void ti_span(const TiRow &R, int c, int W, int *x0, int *x1) {
    int s = 0;
    for (int i = 0; i < c; i++) s += R.k[i].w;
    *x0 = W * s / R.total;
    *x1 = W * (s + R.k[c].w) / R.total;
}

static void ti_label(const char *t, bool ru, int layer, bool up, char *o, size_t n) {
    const char *s = nullptr;
    if (!strcmp(t, "SHIFT")) s = "Shf";
    else if (!strcmp(t, "CAPS")) s = "Cap";
    else if (!strcmp(t, "FN")) s = (layer == 1) ? "ABC" : "Fn";
    else if (!strcmp(t, "EDIT")) s = (layer == 2) ? "ABC" : "Edt";
    else if (!strcmp(t, "LANG")) s = ru ? "EN" : "RU";
    else if (!strcmp(t, "DEL")) s = "Del";
    else if (!strcmp(t, "ENTER")) s = "Ent";
    else if (!strcmp(t, "OK")) s = "OK";
    else if (!strcmp(t, "EXIT")) s = "Esc";
    else if (!strcmp(t, "LEFT")) s = "<";
    else if (!strcmp(t, "RIGHT")) s = ">";
    else if (!strcmp(t, "UP")) s = "Up";
    else if (!strcmp(t, "DOWN")) s = "Dn";
    else if (!strcmp(t, "HOME")) s = "Hom";
    else if (!strcmp(t, "END")) s = "End";
    else if (!strcmp(t, "MARK")) s = "Sel";
    else if (!strcmp(t, "ALL")) s = "All";
    else if (!strcmp(t, "COPY")) s = "Cpy";
    else if (!strcmp(t, "CUT")) s = "Cut";
    else if (!strcmp(t, "PASTE")) s = "Pst";
    else if (!strcmp(t, "CLIP")) s = "Clp";
    else if (!strcmp(t, "TAB")) s = "Tab";
    else if (!strcmp(t, "CLEAR")) s = "Clr";
    if (s) { snprintf(o, n, "%s", s); return; }
    snprintf(o, n, "%s", t);
    if (up) ti_upper(o);
}

// a small list picker (A = next, B = choose, A+B hold = -1)
static int ti_pick(const char *title, const char *const items[], int n, int sel) {
    const int W = M5.Display.width(), H = M5.Display.height();
    const uint32_t bg = k85_get_bg(), fg = k85_get_fg(), accent = k85_get_accent();
    const int ly0 = 24, lh = 20;
    int vis = (H - ly0) / lh;
    if (vis < 1) vis = 1;
    int top = 0;
    auto draw = [&]() {
        if (sel < top) top = sel;
        else if (sel >= top + vis) top = sel - vis + 1;
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 8);
        M5.Display.print(title);
        M5.Display.setTextSize(2);
        int end = top + vis;
        if (end > n) end = n;
        for (int i = top; i < end; i++) {
            const int y = ly0 + (i - top) * lh;
            const bool s = (i == sel);
            if (s) M5.Display.fillRect(0, y - 1, W, lh, accent);
            M5.Display.setTextColor(s ? 0x000000 : fg, s ? accent : bg);
            M5.Display.setCursor(4, y + 1);
            M5.Display.print(items[i]);
        }
        k85_status_bar_overlay();
    };
    draw();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return -1; }
        if (k85_btn_a_pressed()) { k85_wake_screen(); sel = (sel + 1) % n; draw(); }
        if (k85_btn_b_pressed()) { k85_wake_screen(); return sel; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static bool ti_run(const char *prompt, char *text, size_t cap, char *out, size_t out_size, bool ml) {
    TiKb kb;
    int st_row = 1, st_col = 0;
    int cur = (int)strlen(text), sel = -1;
    int ws = 0, vl = 0;
    int64_t a_press_start = 0;
    bool a_was_down = false;
    bool a_hold_triggered = false;
    const int hold_ms = 400;
    int64_t last_imu_step_ms = 0;
    const int64_t imu_step_cooldown_ms = 220;
    const float imu_threshold = 0.35f;
    bool is_ru = false, caps = false, shift = false;
    int layer = 0;
    int64_t last_activity_ms = k85_ti_ticks_ms();
    bool kb_hidden = false;
    const char *toast = nullptr;
    int64_t toast_until = 0;

    const int W = M5.Display.width();
    const int H = M5.Display.height();
    const uint32_t bg = k85_get_bg();
    const uint32_t fg = k85_get_fg();
    const uint32_t accent = k85_get_accent();

    const int preview_rows = 3;
    const int ml_cols = (W - 8) / 6;
    const int top = ml ? (14 + preview_rows * 10 + 4) : 38;
    int row_h = (H - top - 2) / 5;
    if (row_h < 12) row_h = 12;

    auto rebuild = [&]() {
        ti_build(kb, layer == 1 ? TI_SYM : (layer == 2 ? TI_EDIT : (is_ru ? TI_RU : TI_EN)));
        if (st_row > 4) st_row = 4;
        if (st_col >= kb.r[st_row].n) st_col = kb.r[st_row].n - 1;
    };
    rebuild();

    auto sel_range = [&](int *a, int *b) -> bool {
        if (sel < 0 || sel == cur) return false;
        *a = sel < cur ? sel : cur;
        *b = sel < cur ? cur : sel;
        return true;
    };
    auto del_sel = [&]() -> bool {
        int a = 0, b = 0;
        if (!sel_range(&a, &b)) return false;
        ti_del(text, a, b);
        cur = a;
        sel = -1;
        return true;
    };
    auto set_toast = [&](const char *s) { toast = s; toast_until = k85_ti_ticks_ms() + 1200; };

    auto draw_text = [&]() {
        int sa = 0, sb = 0;
        const bool hs = sel_range(&sa, &sb);
        if (!ml) {
            const int cap_cp = (W - 8) / 12;
            int offs[132];
            int n = 0;
            for (int i = 0; text[i] && n < 128; ) { offs[n++] = i; i = ti_next(text, i); }
            offs[n] = (int)strlen(text);
            int ci = 0;
            while (ci < n && offs[ci] < cur) ci++;
            if (ci < ws) ws = ci;
            if (ci > ws + cap_cp - 1) ws = ci - cap_cp + 1;
            if (ws < 0) ws = 0;
            M5.Display.setTextSize(2);
            for (int i = ws; i < n && i < ws + cap_cp; i++) {
                const int x = 4 + (i - ws) * 12;
                const bool s = hs && offs[i] >= sa && offs[i] < sb;
                char ch[8];
                int l = offs[i + 1] - offs[i];
                if (l > 7) l = 7;
                memcpy(ch, text + offs[i], (size_t)l);
                ch[l] = 0;
                if (s) M5.Display.fillRect(x, 16, 12, 16, accent);
                M5.Display.setTextColor(s ? 0x000000 : fg, s ? accent : bg);
                M5.Display.setCursor(x, 16);
                M5.Display.print(ch);
            }
            M5.Display.fillRect(4 + (ci - ws) * 12, 16, 2, 16, accent);
            return;
        }
        ti_layout(text, ml_cols);
        const int cl = ti_line_of(cur);
        if (cl < vl) vl = cl;
        if (cl > vl + preview_rows - 1) vl = cl - preview_rows + 1;
        if (vl < 0) vl = 0;
        M5.Display.setTextSize(1);
        for (int r = 0; r < preview_rows; r++) {
            const int l = vl + r;
            if (l >= s_nl) break;
            const int y = 14 + r * 10;
            int i = s_lstart[l];
            const int e = (l + 1 < s_nl) ? (int)s_lstart[l + 1] : (int)strlen(text);
            int col = 0;
            while (i < e && text[i] != '\n') {
                const int nx = ti_next(text, i);
                char ch[8];
                int len = nx - i;
                if (len > 7) len = 7;
                memcpy(ch, text + i, (size_t)len);
                ch[len] = 0;
                const int x = 4 + col * 6;
                const bool s = hs && i >= sa && i < sb;
                if (s) M5.Display.fillRect(x, y, 6, 9, accent);
                M5.Display.setTextColor(s ? 0x000000 : fg, s ? accent : bg);
                M5.Display.setCursor(x, y);
                M5.Display.print(ch);
                i = nx;
                col++;
            }
            if (l == cl) {
                const int cc = ti_cpcount(text, s_lstart[l], cur);
                M5.Display.fillRect(4 + cc * 6, y, 1, 9, accent);
            }
        }
    };

    auto draw = [&]() {
        if (st_col >= kb.r[st_row].n) st_col = kb.r[st_row].n - 1;
        const bool up = (caps != shift);

        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 2);
        M5.Display.printf("%s [%s/%s]", prompt, is_ru ? "RU" : "EN",
                          layer == 1 ? "sym" : (layer == 2 ? "edit" : (caps ? "CAPS" : (shift ? "Shift" : "low"))));
        if (toast) {
            M5.Display.setTextColor(0xFFD54F, bg);
            M5.Display.setCursor(W - 72, 2);
            M5.Display.print(toast);
        }
        draw_text();

        if (kb_hidden) {
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(0x666666, bg);
            const char *hint = "(press any button)";
            const int hx = (W - (int)strlen(hint) * 6) / 2;
            M5.Display.setCursor(hx, H - 14);
            M5.Display.print(hint);
            k85_status_bar_overlay();
            return;
        }

        for (int r = 0; r < 5; r++) {
            const TiRow &R = kb.r[r];
            const int y = top + r * row_h;
            for (int c = 0; c < R.n; c++) {
                int x0, x1;
                ti_span(R, c, W, &x0, &x1);
                const TiKey &K = R.k[c];
                const bool is_sel = (r == st_row && c == st_col);
                const bool on = (!strcmp(K.t, "CAPS") && caps) || (!strcmp(K.t, "SHIFT") && shift) ||
                                (!strcmp(K.t, "FN") && layer == 1) || (!strcmp(K.t, "EDIT") && layer == 2) ||
                                (!strcmp(K.t, "MARK") && sel >= 0);
                const uint32_t bgc = is_sel ? accent : (on ? 0x3A5F9A : 0x222222);
                const uint32_t fgc = is_sel ? 0x000000 : fg;
                M5.Display.fillRect(x0 + 1, y + 1, x1 - x0 - 2, row_h - 2, bgc);
                if (!strcmp(K.t, "SPACE")) {
                    M5.Display.fillRect(x0 + 8, y + row_h / 2 - 1, x1 - x0 - 16, 2, fgc);
                    continue;
                }
                char lab[12];
                ti_label(K.t, is_ru, layer, up, lab, sizeof(lab));
                M5.Display.setTextSize(1);
                M5.Display.setTextColor(fgc, bgc);
                int tx = x0 + ((x1 - x0) - ti_u8len(lab) * 6) / 2;
                if (tx < x0) tx = x0;
                int ty = y + (row_h - 8) / 2;
                if (ty < y) ty = y;
                M5.Display.setCursor(tx, ty);
                M5.Display.print(lab);
            }
        }
        k85_status_bar_overlay();
    };

    auto clip_menu = [&]() {
        char lab[K85_CLIP_SLOTS][40];
        const char *it[K85_CLIP_SLOTS + 2];
        int n = 0;
        for (int i = 0; i < k85_clip_count() && i < K85_CLIP_SLOTS; i++) {
            const char *c = k85_clip_get(i);
            if (!c) break;
            ti_first_line(c, lab[i], sizeof(lab[i]), 16);
            if (!lab[i][0]) snprintf(lab[i], sizeof(lab[i]), "(blank)");
            it[n++] = lab[i];
        }
        const int nclip = n;
        it[n++] = "Clear clipboard";
        it[n++] = "Back";
        const int r = ti_pick("Clipboard", it, n, 0);
        if (r < 0 || r == n - 1) return;
        if (r == nclip) { k85_clip_clear(); set_toast("Clip empty"); return; }
        const char *c = k85_clip_get(r);
        if (c) {
            del_sel();
            if (ti_ins(text, cap, &cur, c)) set_toast("Pasted"); else set_toast("No room");
            sel = -1;
        }
    };

    draw();
    while (true) {
        k85_input_update();
        const int64_t now = k85_ti_ticks_ms();

        if (toast && now >= toast_until) { toast = nullptr; draw(); }

        float imu_ax = 0, imu_ay = 0, imu_az = 0;
        if (g_config.kbd_nav_mode == 1) {
            M5.Imu.getAccel(&imu_ax, &imu_ay, &imu_az);
        }
        const bool imu_tilted = (g_config.kbd_nav_mode == 1) &&
                                (fabsf(imu_ax) > imu_threshold || fabsf(imu_ay) > imu_threshold);
        const bool any_input = k85_btn_a_is_down() || k85_btn_b_is_down() || imu_tilted;
        if (any_input) {
            if (kb_hidden) {
                kb_hidden = false;
                last_activity_ms = now;
                draw();
                vTaskDelay(pdMS_TO_TICKS(30));
                continue;   // this press only wakes the keyboard, it types nothing
            }
            last_activity_ms = now;
        } else if (!kb_hidden && (now - last_activity_ms) >= K85_TI_KB_IDLE_MS) {
            kb_hidden = true;
            draw();
        }

        if (g_config.kbd_nav_mode == 1) {
            if (now - last_imu_step_ms >= imu_step_cooldown_ms) {
                bool moved = false;
                const int cn = kb.r[st_row].n;
                if (imu_ax > imu_threshold) { st_col = (st_col + 1) % cn; moved = true; }
                else if (imu_ax < -imu_threshold) { st_col = (st_col - 1 + cn) % cn; moved = true; }
                else if (imu_ay > imu_threshold) { st_row = (st_row + 1) % 5; moved = true; }
                else if (imu_ay < -imu_threshold) { st_row = (st_row + 4) % 5; moved = true; }
                if (moved) {
                    if (st_col > kb.r[st_row].n - 1) st_col = kb.r[st_row].n - 1;
                    k85_wake_screen();
                    last_imu_step_ms = now;
                    draw();
                }
            }
        } else {
            const bool a_down_now = k85_btn_a_is_down();
            if (a_down_now && !a_was_down) {
                a_press_start = now;
                a_hold_triggered = false;
            }
            if (a_down_now && !a_hold_triggered && (now - a_press_start) >= hold_ms) {
                st_row = (st_row + 1) % 5;
                if (st_col > kb.r[st_row].n - 1) st_col = kb.r[st_row].n - 1;
                a_hold_triggered = true;
                draw();
            }
            if (!a_down_now && a_was_down && !a_hold_triggered) {
                k85_wake_screen();
                st_col = (st_col + 1) % kb.r[st_row].n;
                draw();
            }
            a_was_down = a_down_now;
        }

        bool tapped = false;
        {
            int tx_ = 0, ty_ = 0;
            if (k85_pointer_take(&tx_, &ty_)) {
                if (kb_hidden) {
                    kb_hidden = false;
                    last_activity_ms = now;
                    draw();
                } else if (ty_ >= top) {
                    const int r_ = (ty_ - top) / row_h;
                    if (r_ >= 0 && r_ < 5) {
                        const TiRow &R = kb.r[r_];
                        for (int c_ = 0; c_ < R.n; c_++) {
                            int x0, x1;
                            ti_span(R, c_, W, &x0, &x1);
                            if (tx_ >= x0 && tx_ < x1) { st_row = r_; st_col = c_; tapped = true; break; }
                        }
                    }
                }
            }
        }

        if (k85_btn_b_pressed() || tapped) {
            k85_wake_screen();
            last_activity_ms = now;
            const char *key = kb.r[st_row].k[st_col].t;
            const int len = (int)strlen(text);
            bool accept = false;
            if (!strcmp(key, "SPACE")) {
                del_sel(); ti_ins(text, cap, &cur, " "); sel = -1;
            } else if (!strcmp(key, "TAB")) {
                del_sel(); ti_ins(text, cap, &cur, "  "); sel = -1;
            } else if (!strcmp(key, "DEL")) {
                if (!del_sel() && cur > 0) {
                    const int p = ti_prev(text, cur);
                    ti_del(text, p, cur);
                    cur = p;
                }
                sel = -1;
            } else if (!strcmp(key, "ENTER")) {
                if (ml) { del_sel(); ti_ins(text, cap, &cur, "\n"); sel = -1; }
                else accept = true;
            } else if (!strcmp(key, "OK")) {
                accept = true;
            } else if (!strcmp(key, "EXIT")) {
                return false;
            } else if (!strcmp(key, "SHIFT")) {
                shift = !shift;
            } else if (!strcmp(key, "CAPS")) {
                caps = !caps;
            } else if (!strcmp(key, "FN")) {
                layer = (layer == 1) ? 0 : 1;
                rebuild();
            } else if (!strcmp(key, "EDIT")) {
                layer = (layer == 2) ? 0 : 2;
                rebuild();
            } else if (!strcmp(key, "LANG")) {
                is_ru = !is_ru;
                layer = 0;
                rebuild();
            } else if (!strcmp(key, "LEFT")) {
                cur = ti_prev(text, cur);
            } else if (!strcmp(key, "RIGHT")) {
                cur = ti_next(text, cur);
            } else if (!strcmp(key, "HOME")) {
                cur = ml ? ti_home(text, cur) : 0;
            } else if (!strcmp(key, "END")) {
                cur = ml ? ti_end(text, cur) : len;
            } else if (!strcmp(key, "UP")) {
                cur = ml ? ti_vmove(text, cur, ml_cols, -1) : 0;
            } else if (!strcmp(key, "DOWN")) {
                cur = ml ? ti_vmove(text, cur, ml_cols, 1) : len;
            } else if (!strcmp(key, "MARK")) {
                sel = (sel >= 0) ? -1 : cur;
            } else if (!strcmp(key, "ALL")) {
                sel = 0;
                cur = len;
            } else if (!strcmp(key, "COPY") || !strcmp(key, "CUT")) {
                int a = 0, b = len;
                sel_range(&a, &b);
                if (b > a) {
                    k85_clip_push(text + a, (size_t)(b - a));
                    if (!strcmp(key, "CUT")) { ti_del(text, a, b); cur = a; set_toast("Cut to clip"); }
                    else set_toast("Copied");
                } else {
                    set_toast("Empty text");
                }
                sel = -1;
            } else if (!strcmp(key, "PASTE")) {
                const char *c = k85_clip_get(0);
                if (!c) {
                    set_toast("Clip empty");
                } else {
                    del_sel();
                    if (ti_ins(text, cap, &cur, c)) set_toast("Pasted"); else set_toast("No room");
                }
                sel = -1;
            } else if (!strcmp(key, "CLIP")) {
                clip_menu();
            } else if (!strcmp(key, "CLEAR")) {
                text[0] = 0;
                cur = 0;
                sel = -1;
            } else {
                char b[8];
                snprintf(b, sizeof(b), "%s", key);
                if (caps != shift) ti_upper(b);
                del_sel();
                ti_ins(text, cap, &cur, b);
                sel = -1;
                shift = false;
            }
            if (accept) {
                snprintf(out, out_size, "%s", text);
                return true;
            }
            draw();
        }
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            snprintf(out, out_size, "%s", text);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

bool k85_text_input(const char *prompt, const char *initial, char *out, size_t out_size) {
    char text[128];
    snprintf(text, sizeof(text), "%s", initial ? initial : "");
    const bool r = ti_run(prompt, text, sizeof(text), out, out_size, false);
    ti_wipe(text, sizeof(text));
    return r;
}

bool k85_text_input_multiline(const char *prompt, const char *initial, char *out, size_t out_size) {
    static EXT_RAM_BSS_ATTR char text[2048];
    snprintf(text, sizeof(text), "%s", initial ? initial : "");
    const bool r = ti_run(prompt, text, sizeof(text), out, out_size, true);
    ti_wipe(text, sizeof(text));
    return r;
}

void k85_area_show(const char *const lines[], int count, const char *title) {
    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(accent, bg);
    M5.Display.setCursor(4, 2);
    M5.Display.print(title);

    int y = 16;
    for (int i = 0; i < count; i++) {
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, y);
        M5.Display.print(lines[i]);
        y += 12;
    }
    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(4, M5.Display.height() - 12);
    M5.Display.print("A+B=back");
    k85_status_bar_overlay();

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
