// K85CLIP1: system clipboard (the 5 latest copies, kept in PSRAM)
#include "clipboard.h"
#include "esp_attr.h"
#include <cstring>

static EXT_RAM_BSS_ATTR char s_clip[K85_CLIP_SLOTS][K85_CLIP_MAX];
static int s_n = 0;

void k85_clip_push(const char *text, size_t n) {
    if (!text || n == 0) return;
    if (n > K85_CLIP_MAX - 1) {
        n = K85_CLIP_MAX - 1;
        while (n > 0 && (((unsigned char)text[n]) & 0xC0) == 0x80) n--;   // do not cut a UTF-8 character
    }
    if (n == 0) return;
    int d = -1;
    for (int i = 0; i < s_n; i++) {
        if (strlen(s_clip[i]) == n && memcmp(s_clip[i], text, n) == 0) { d = i; break; }
    }
    if (d >= 0) {
        if (d > 0) memmove(s_clip[1], s_clip[0], (size_t)d * K85_CLIP_MAX);
    } else {
        const int cnt = (s_n < K85_CLIP_SLOTS) ? s_n : K85_CLIP_SLOTS - 1;
        if (cnt > 0) memmove(s_clip[1], s_clip[0], (size_t)cnt * K85_CLIP_MAX);
        if (s_n < K85_CLIP_SLOTS) s_n++;
    }
    memcpy(s_clip[0], text, n);
    s_clip[0][n] = 0;
}

const char *k85_clip_get(int i) {
    if (i < 0 || i >= s_n) return nullptr;
    return s_clip[i];
}

int k85_clip_count(void) { return s_n; }

void k85_clip_clear(void) {
    s_n = 0;
    memset(s_clip, 0, sizeof(s_clip));
}