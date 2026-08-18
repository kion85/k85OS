#pragma once
#include <stdbool.h>

#define K85_MAX_PROFILES 3

typedef struct {
    char name[24];
    int theme_idx;
    int brightness_active;
    int sound_volume;
    bool sound_muted;
    int battery_mode_idx;
    int bootstyle_idx;
    bool bg_gradient_enabled;
    char wifi_ssid[64]; // "" = не трогать WiFi
} k85_profile_t;

void k85_profiles_defaults(k85_profile_t profiles[K85_MAX_PROFILES]);
void k85_profile_apply(int idx);
