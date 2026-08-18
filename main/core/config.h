#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/profiles.h"

// ---------- LittleFS ----------
#define K85_LITTLEFS_PART_LABEL "storage"
#define K85_LITTLEFS_BASE_PATH  "/littlefs"
#define CONFIG_FILE_PATH        "/littlefs/k85os_config.json"

#define K85_MAX_WIFI_NETWORKS 3

// ---------- РЎС‚СЂСѓРєС‚СѓСЂС‹ РєРѕРЅС„РёРіР° (1:1 СЃ РѕСЂРёРіРёРЅР°Р»СЊРЅС‹Рј JSON k85OS v4.1) ----------
typedef struct {
    int game_2048;
    int tetris;
    int snake;
    int memory;
    int reaction;
    int flappy;
    int balance;
    int pong;
} k85_high_scores_t;

typedef struct {
    char ssid[64];
    char password[64];
} k85_wifi_net_t;

typedef struct {
    int theme_idx;
    int battery_mode_idx;
    int brightness_active;
    int device_name_idx;
    int rotation;
    int bootstyle_idx;
    int kb_layout;
    int sound_volume;
    int utc_offset; // -12..+12, часовой пояс относительно UTC
    bool ota_locked;   // true = блокирует OTA-обновления по воздуху
    bool wifi_disabled; // true = модуль WiFi выключен пользователем
    bool bt_disabled;   // true = модуль Bluetooth выключен пользователем
    bool lock_enabled;
    char lock_password[100]; // hex: [16 байт соли][32 байта PBKDF2-хеша]
    bool post_beep_enabled;
    int alarm_volume_idx;
    bool sound_muted;
    uint32_t sb_flags; // битовые флаги статус-бара главного меню
    uint32_t sb_bg_color; // 0xFFFFFFFF = использовать фон темы
    int ap_channel; // 1-13, канал точки доступа Hotspot
    bool ap_open;
    bool bg_gradient_enabled;
    int lock_shape; // 0=circle 1=square 2=mixed
    uint32_t lock_particle_color; // 0xFFFFFFFF = авто-палитра
    bool grub_enabled;
    int default_boot_choice; // 0=Normal 1=BIOS 2=Test Mode (когда GRUB выключен)   // true = открытая сеть без пароля, false = WPA2
    uint32_t bios_bg_color;   // 0xFFFFFFFF = дефолт/градиент
    uint32_t bios_hl_color;   // 0xFFFFFFFF = дефолт (акцент темы)
    uint32_t bios_text_color; // 0xFFFFFFFF = дефолт (текст темы)
    int bios_selection_style; // 0=заливка 1=только стрелка
    k85_high_scores_t high_scores;

    // "wifi" (С‚РµРєСѓС‰Р°СЏ СЃРµС‚СЊ)
    char wifi_ssid[64];
    char wifi_password[64];
    bool wifi_saved;

    // "wifi_networks[]" (РґРѕ 3 СЃРѕС…СЂР°РЅС‘РЅРЅС‹С…)
    k85_wifi_net_t wifi_networks[K85_MAX_WIFI_NETWORKS];
    int wifi_networks_count;

    // "step_*"
    int step_count;
    int step_record;
    char step_date[16];

    // SSH server
    bool ssh_enabled;
    char ssh_username[32];
    char ssh_password_hash[65]; // hex SHA-256
    // Profiles (Home/Work/Eco)
    k85_profile_t profiles[K85_MAX_PROFILES];
    int active_profile_idx;
} k85_config_t;

extern k85_config_t g_config;

// ---------- API ----------
bool k85_fs_init(void);              // РјРѕРЅС‚РёСЂРѕРІР°РЅРёРµ LittleFS
bool k85_fs_info(size_t *total_bytes, size_t *used_bytes); // СЂР°Р·РјРµСЂ LittleFS РґР»СЏ System Info
void k85_config_defaults(k85_config_t *cfg);
bool k85_config_load(void);          // load + deep-merge РґРµС„РѕР»С‚РѕРІ
bool k85_config_save(void);

int  k85_get_high_score(const char *game);
bool k85_set_high_score_if_better(const char *game, int value);
bool k85_set_low_score_if_better(const char *game, int value);

int  k85_get_sound_volume(void);
void k85_set_sound_volume(int v);














