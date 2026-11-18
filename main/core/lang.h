#pragma once
// K85RU1: Russian font + language switch + small dictionary for menus
#include <stddef.h>
#include <stdbool.h>
namespace lgfx { inline namespace v1 { struct IFont; } }
const lgfx::IFont *k85_ru_font(void);                 // 6x8 font: Latin as before + Russian letters
const char *k85_tr(const char *s);                    // English menu text -> Russian when the language is RU
int  k85_u8len(const char *s);                        // number of characters (not bytes) in a UTF-8 string
void k85_u8trunc(char *dst, size_t n, const char *src, int maxch);   // copy at most maxch characters
bool k85_lang_ru(void);
void k85_tr_install(void);                           // hooks the phrase translator into the M5GFX text output
void k85_lang_set(bool ru);
void k85_lang_cmd(const char *arg, char *out, size_t n);   // shell: lang [ru|en]