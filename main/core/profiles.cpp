#include "core/profiles.h"
#include "core/config.h"
#include "core/theme.h"
#include "core/sound.h"
#include "net/wifi.h"
#include "M5Unified.h"
#include <cstring>
#include <cstdio>

void k85_profiles_defaults(k85_profile_t profiles[K85_MAX_PROFILES]) {
    snprintf(profiles[0].name, sizeof(profiles[0].name), "Home");
    profiles[0].theme_idx = 0;
    profiles[0].brightness_active = 100;
    profiles[0].sound_volume = 80;
    profiles[0].sound_muted = false;
    profiles[0].battery_mode_idx = 0;
    profiles[0].bootstyle_idx = 0;
    profiles[0].bg_gradient_enabled = true;
    profiles[0].wifi_ssid[0] = '\0';

    snprintf(profiles[1].name, sizeof(profiles[1].name), "Work");
    profiles[1].theme_idx = 0;
    profiles[1].brightness_active = 50;
    profiles[1].sound_volume = 0;
    profiles[1].sound_muted = true;
    profiles[1].battery_mode_idx = 0;
    profiles[1].bootstyle_idx = 2;
    profiles[1].bg_gradient_enabled = false;
    profiles[1].wifi_ssid[0] = '\0';

    snprintf(profiles[2].name, sizeof(profiles[2].name), "Eco");
    profiles[2].theme_idx = 0;
    profiles[2].brightness_active = 25;
    profiles[2].sound_volume = 30;
    profiles[2].sound_muted = false;
    profiles[2].battery_mode_idx = 1;
    profiles[2].bootstyle_idx = 2;
    profiles[2].bg_gradient_enabled = false;
    profiles[2].wifi_ssid[0] = '\0';
}

// ВАЖНО: если функция применения темы называется иначе чем k85_apply_theme(),
// поправь строку ниже под реальное имя из core/theme.h
void k85_profile_apply(int idx) {
    if (idx < 0 || idx >= K85_MAX_PROFILES) return;

    k85_profile_t *p = &g_config.profiles[idx];

    g_config.theme_idx = p->theme_idx;
    g_config.brightness_active = p->brightness_active;
    M5.Display.setBrightness(p->brightness_active);

    k85_set_sound_volume(p->sound_muted ? 0 : p->sound_volume);

    g_config.battery_mode_idx = p->battery_mode_idx;
    g_config.bootstyle_idx = p->bootstyle_idx;
    g_config.bg_gradient_enabled = p->bg_gradient_enabled;

    // k85_apply_theme(); // раскомментируй/поправь имя, если тема требует явного refresh

    if (p->wifi_ssid[0] != '\0' && strcmp(g_config.wifi_ssid, p->wifi_ssid) != 0) {
        for (int i = 0; i < g_config.wifi_networks_count; i++) {
            if (strcmp(g_config.wifi_networks[i].ssid, p->wifi_ssid) == 0) {
                strncpy(g_config.wifi_ssid, g_config.wifi_networks[i].ssid, sizeof(g_config.wifi_ssid));
                strncpy(g_config.wifi_password, g_config.wifi_networks[i].password, sizeof(g_config.wifi_password));
                g_config.wifi_saved = true;
                k85_wifi_connect_saved();
                break;
            }
        }
    }

    g_config.active_profile_idx = idx;
    k85_config_save();
}
