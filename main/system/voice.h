#pragma once
#include <stdbool.h>

void k85_voice_init(void);               // необязательно: всё поднимается лениво
bool k85_voice_enabled(void);
void k85_voice_set(bool on);             // говорит "on"/"off", громкость ~55%
void k85_voice_say(const char *text);    // no-op, если режим выключен
bool k85_voice_pack_present(void);
bool k85_voice_download(const char *url);
void k85_voice_ensure_pack(void);        // фоновая докачка voice.pak, когда появится WiFi
bool k85_voice_take_notify(void);   // true once, right after the voice pack finished downloading
void k85_voice_flash_ready(void);    // green "Voice pack ready" screen (UI thread only)
