// K85NOTES1: Notes app (main menu -> Notes).
// Storage: one file on LittleFS, notes separated by byte 0x1E.
// A = next item, B = select, A+B (hold) = back / exit.
#include "notes.h"
#include "theme.h"
#include "battery.h"
#include "status_bar.h"
#include "power.h"
#include "input.h"
#include "text_input.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define NOTES_FILE "/littlefs/notes.dat"
#define NOTES_MAX 24
#define NOTE_LEN 768

static char *s_notes[NOTES_MAX];
static int s_count = 0;

static void *nalloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n);
    return p;
}

static void notes_free(void) {
    for (int i = 0; i < s_count; i++) { free(s_notes[i]); s_notes[i] = nullptr; }
    s_count = 0;
}

static void notes_load(void) {
    notes_free();
    FILE *f = fopen(NOTES_FILE, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    const long cap = (long)NOTES_MAX * (NOTE_LEN + 1);
    if (sz <= 0) { fclose(f); return; }
    if (sz > cap) sz = cap;
    char *buf = (char *)nalloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    size_t pos = 0;
    while (pos < n && s_count < NOTES_MAX) {
        const char *e = (const char *)memchr(buf + pos, 0x1E, n - pos);
        size_t len = e ? (size_t)(e - (buf + pos)) : n - pos;
        size_t next = pos + len + (e ? 1 : 0);
        if (len > NOTE_LEN - 1) len = NOTE_LEN - 1;
        if (len > 0) {
            char *t = (char *)nalloc(len + 1);
            if (!t) break;
            memcpy(t, buf + pos, len);
            t[len] = 0;
            s_notes[s_count++] = t;
        }
        pos = next;
    }
    free(buf);
}

static bool notes_save(void) {
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", NOTES_FILE);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = true;
    for (int i = 0; i < s_count; i++) {
        size_t l = strlen(s_notes[i]);
        if (fwrite(s_notes[i], 1, l, f) != l) ok = false;
        if (fputc(0x1E, f) == EOF) ok = false;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return false; }
    remove(NOTES_FILE);
    return rename(tmp, NOTES_FILE) == 0;
}

static void note_msg(const char *s, uint32_t col) {
    int H = M5.Display.height(), W = M5.Display.width();
    M5.Display.fillRect(0, H / 2 - 14, W, 28, 0x000000);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(col, 0x000000);
    M5.Display.setCursor(6, H / 2 - 8);
    M5.Display.print(s);
    vTaskDelay(pdMS_TO_TICKS(1200));
}

// first line of s, at most maxcp UTF-8 characters ("..", if cut)
static void first_line(const char *s, char *dst, size_t dsz, int maxcp) {
    size_t o = 0;
    int cp = 0;
    while (*s && *s != '\n' && *s != '\r') {
        if (cp >= maxcp) {
            if (o + 2 < dsz) { dst[o++] = '.'; dst[o++] = '.'; }
            break;
        }
        unsigned char c = (unsigned char)*s;
        size_t l = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (strnlen(s, l) < l) break;
        if (o + l + 1 > dsz) break;
        memcpy(dst + o, s, l);
        o += l; s += l; cp++;
    }
    dst[o] = 0;
}

// A = next, B = choose, A+B hold = -1
static int pick(const char *title, const char *const items[], int n, int sel) {
    const int W = M5.Display.width(), H = M5.Display.height();
    const uint32_t bg = k85_get_bg(), fg = k85_get_fg(), accent = k85_get_accent();
    const int ty = 16, ly0 = 28, lh = 22;
    int vis = (H - ly0) / lh;
    if (vis < 1) vis = 1;
    int top = 0;
    auto draw = [&]() {
        if (sel < top) top = sel;
        else if (sel >= top + vis) top = sel - vis + 1;
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, ty);
        M5.Display.print(title);
        M5.Display.setTextSize(2);
        int end = top + vis;
        if (end > n) end = n;
        for (int i = top; i < end; i++) {
            int y = ly0 + (i - top) * lh;
            bool s = (i == sel);
            if (s) M5.Display.fillRect(0, y - 2, W, lh, accent);
            M5.Display.setTextColor(s ? 0x000000 : fg, s ? accent : bg);
            M5.Display.setCursor(4, y);
            M5.Display.print(items[i]);
        }
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xAAAAAA, bg);
        if (top > 0) { M5.Display.setCursor(W - 10, ly0); M5.Display.print("^"); }
        if (end < n) { M5.Display.setCursor(W - 10, H - 10); M5.Display.print("v"); }
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

// read-only viewer: A = page down (wraps to top), B or A+B = back
static void view_note(const char *txt) {
    const int W = M5.Display.width(), H = M5.Display.height();
    const uint32_t bg = k85_get_bg(), fg = k85_get_fg(), accent = k85_get_accent();
    const int cols = 38, lh = 10, y0 = 16;
    struct Ln { uint16_t off, len; };
    Ln lines[96];
    int nl = 0;
    size_t n = strlen(txt), i = 0;
    while (i <= n && nl < 96) {
        size_t start = i;
        int cp = 0;
        while (i < n && txt[i] != '\n' && cp < cols) {
            unsigned char c = (unsigned char)txt[i];
            size_t l = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            if (i + l > n) l = n - i;
            i += l; cp++;
        }
        lines[nl].off = (uint16_t)start;
        lines[nl].len = (uint16_t)(i - start);
        nl++;
        if (i < n && txt[i] == '\n') i++;
        else if (i >= n) break;
    }
    int vis = (H - y0) / lh;
    if (vis < 1) vis = 1;
    int top = 0;
    auto draw = [&]() {
        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(fg, bg);
        for (int r = 0; r < vis && top + r < nl; r++) {
            char tmp[160];
            size_t len = lines[top + r].len;
            if (len > sizeof(tmp) - 1) len = sizeof(tmp) - 1;
            memcpy(tmp, txt + lines[top + r].off, len);
            tmp[len] = 0;
            M5.Display.setCursor(2, y0 + r * lh);
            M5.Display.print(tmp);
        }
        if (nl > vis) {
            int bh = H * vis / nl;
            if (bh < 6) bh = 6;
            int by = (H - bh) * top / (nl - vis);
            M5.Display.fillRect(W - 3, by, 3, bh, accent);
        }
        k85_status_bar_overlay();
    };
    draw();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed()) {
            k85_wake_screen();
            if (top + vis >= nl) top = 0;
            else {
                top += vis - 1;
                if (top > nl - vis) top = nl - vis;
                if (top < 0) top = 0;
            }
            draw();
        }
        if (k85_btn_b_pressed()) { k85_wake_screen(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// idx < 0 = new note (goes to the top of the list). true = list changed
static bool edit_note(int idx) {
    if (idx < 0 && s_count >= NOTES_MAX) { note_msg("Notes full", 0xFF4444); return false; }
    char *ini = (char *)nalloc(NOTE_LEN);
    char *out = (char *)nalloc(NOTE_LEN);
    if (!ini || !out) { free(ini); free(out); note_msg("No memory", 0xFF4444); return false; }
    snprintf(ini, NOTE_LEN, "%s", idx >= 0 ? s_notes[idx] : "");
    out[0] = 0;
    bool ok = k85_text_input_multiline(idx >= 0 ? "Edit note" : "New note", ini, out, NOTE_LEN);
    free(ini);
    bool changed = false;
    if (ok) {
        size_t l = strlen(out);
        while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' ')) out[--l] = 0;
        if (l == 0) {
            note_msg("Empty: not saved", 0xFFAA00);
        } else {
            char *copy = (char *)nalloc(l + 1);
            if (copy) {
                memcpy(copy, out, l + 1);
                if (idx >= 0) {
                    free(s_notes[idx]);
                    s_notes[idx] = copy;
                } else {
                    memmove(&s_notes[1], &s_notes[0], sizeof(char *) * (size_t)s_count);
                    s_notes[0] = copy;
                    s_count++;
                }
                changed = true;
            }
        }
    }
    free(out);
    return changed;
}

void k85_run_notes(void) {
    notes_load();
    int sel = 0;
    while (true) {
        char labels[NOTES_MAX + 1][48];
        const char *items[NOTES_MAX + 1];
        snprintf(labels[0], sizeof(labels[0]), "+ New note");
        items[0] = labels[0];
        for (int i = 0; i < s_count; i++) {
            first_line(s_notes[i], labels[i + 1], sizeof(labels[i + 1]), 16);
            if (!labels[i + 1][0]) snprintf(labels[i + 1], sizeof(labels[i + 1]), "(empty)");
            items[i + 1] = labels[i + 1];
        }
        int n = s_count + 1;
        if (sel >= n) sel = n - 1;
        int r = pick("Notes", items, n, sel);
        if (r < 0) break;
        sel = r;
        if (r == 0) {
            if (edit_note(-1)) {
                if (!notes_save()) note_msg("Save failed", 0xFF4444);
                sel = 1;
            }
            continue;
        }
        int idx = r - 1;
        char title[96];
        first_line(s_notes[idx], title, sizeof(title), 30);
        const char *acts[] = {"View note", "Edit note", "Delete note", "Back to list"};
        int a = pick(title, acts, 4, 0);
        if (a == 0) {
            view_note(s_notes[idx]);
        } else if (a == 1) {
            if (edit_note(idx) && !notes_save()) note_msg("Save failed", 0xFF4444);
        } else if (a == 2) {
            const char *yn[] = {"Keep it", "Yes, delete"};
            if (pick("Delete note?", yn, 2, 0) == 1) {
                free(s_notes[idx]);
                memmove(&s_notes[idx], &s_notes[idx + 1], sizeof(char *) * (size_t)(s_count - idx - 1));
                s_count--;
                if (!notes_save()) note_msg("Save failed", 0xFF4444);
            }
        }
    }
    notes_free();
}