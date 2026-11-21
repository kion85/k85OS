#pragma once
#include <stddef.h>
// K85CLIP1: system clipboard, the 5 latest copies (newest = 0). Used by the on-screen keyboard (Cpy / Cut / Pst / Clp).
#define K85_CLIP_SLOTS 5
#define K85_CLIP_MAX 1024
void k85_clip_push(const char *text, size_t n);   // copy n bytes: they become the newest entry (an identical one moves to the top)
const char *k85_clip_get(int i);                  // 0 = newest, nullptr if there is no such entry
int k85_clip_count(void);
void k85_clip_clear(void);