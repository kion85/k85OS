#include "package_manager.h"
#include "common.h"
#include "list_menu.h"
#include "input.h"
#include "config.h"
#include "../../net/app_repo.h"
#include "../../net/firmware_flash.h"
#include "../../net/wifi.h"
#include "../../core/version.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

// 1 = refuse to flash packages that have no signature URL
#define K85_PM_REQUIRE_SIG 1
// max size of a .thm file (they are one short line of text)
#define K85_THEME_MAX_BYTES 4096

// ---------- PSRAM buffers (freed automatically on every return) ----------
struct PsramDeleter {
    void operator()(void *p) const { heap_caps_free(p); }
};

template <typename T>
static std::unique_ptr<T, PsramDeleter> psram_alloc(void) {
    void *p = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_8BIT);
    return std::unique_ptr<T, PsramDeleter>(static_cast<T *>(p));
}

struct AppList {
    char names[K85_APPREPO_MAX_ASSETS][64];
    char urls[K85_APPREPO_MAX_ASSETS][256];
    char sigs[K85_APPREPO_MAX_ASSETS][256];
};

struct FwList {
    char names[K85_FW_LIST_MAX][64];
    char urls[K85_FW_LIST_MAX][256];
    char sigs[K85_FW_LIST_MAX][256];
};

struct ThemeList {
    char names[K85_APPREPO_MAX_ASSETS][64];
    char urls[K85_APPREPO_MAX_ASSETS][256];
    char labeled[K85_APPREPO_MAX_ASSETS][80];
};

// ---------- helpers ----------
static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void show_and_wait(const char *msg) {
    k85_show_message(msg);
    wait_ab_exit();
}

static bool require_wifi(void) {
    if (!k85_wifi_is_connected()) {
        show_and_wait("WiFi not connected\nA+B=back");
        return false;
    }
    return true;
}

static bool confirm_flash(const char *name) {
    char confirm[200];
    snprintf(confirm, sizeof(confirm),
             "Flash %.40s\ninto spare slot?\n(replaces game/firmware\nalready there)\nB=confirm A+B=cancel", name);
    k85_show_message(confirm);
    k85_wait_ab_release();  // don't reuse the press that opened this screen
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) return true;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// Shared flash flow for Games and Firmware (both go to the spare OTA slot).
static void flash_selected(const char *name, const char *url, const char *sig_url) {
#if K85_PM_REQUIRE_SIG
    if (!sig_url || !sig_url[0]) {
        show_and_wait("No signature for\nthis package.\nRefusing to flash\nA+B=back");
        return;
    }
#endif
    if (!confirm_flash(name)) return;

    k85_show_message("Downloading &\nflashing...\nThis may take a while");

    // The spare slot is about to be overwritten: forget the old name first,
    // so a failed flash never leaves a "valid" name pointing at a broken slot.
    g_config.alt_slot_name[0] = '\0';
    k85_config_save();

    bool ok = k85_fwflash_from_url(url, sig_url, nullptr);
    if (ok) {
        snprintf(g_config.alt_slot_name, sizeof(g_config.alt_slot_name), "%s", name);
        k85_config_save();
        show_and_wait("Flashed to free slot!\nActivate via BIOS ->\nBoot options ->\nAlt Firmware\nA+B=back");
    } else {
        show_and_wait("Download/flash failed\nA+B=back");
    }
}

static bool safe_theme_name(const char *n) {
    size_t len = strlen(n);
    if (len < 5 || len > 40) return false;
    if (strstr(n, "..") || strchr(n, '/') || strchr(n, '\\')) return false;
    return strcmp(n + len - 4, ".thm") == 0;
}

// ---------- Games ----------
static void run_games_category(void) {
    if (!require_wifi()) return;

    auto l = psram_alloc<AppList>();
    if (!l) { show_and_wait("Out of memory\nA+B=back"); return; }

    k85_show_message("Fetching game list...");
    int count = 0;
    if (!k85_apprepo_fetch_app_list(l->names, l->urls, l->sigs, K85_APPREPO_MAX_ASSETS, &count) || count == 0) {
        show_and_wait("No games found\nA+B=back");
        return;
    }

    while (true) {
        char header[80];
        snprintf(header, sizeof(header), "GAMES (slot: %.30s)",
                 g_config.alt_slot_name[0] ? g_config.alt_slot_name : "empty");

        const char *items[K85_APPREPO_MAX_ASSETS + 1];
        for (int i = 0; i < count; i++) items[i] = l->names[i];
        items[count] = "Back";

        int idx = k85_run_list_menu(header, items, count + 1, nullptr);
        if (idx < 0 || idx >= count) return;

        flash_selected(l->names[idx], l->urls[idx], l->sigs[idx]);
    }
}

// ---------- Themes ----------
static void run_themes_category(void) {
    if (!require_wifi()) return;

    auto l = psram_alloc<ThemeList>();
    if (!l) { show_and_wait("Out of memory\nA+B=back"); return; }

    k85_show_message("Fetching theme list...");
    int count = 0;
    if (!k85_apprepo_fetch_uefi_theme_list(l->names, l->urls, K85_APPREPO_MAX_ASSETS, &count) || count == 0) {
        show_and_wait("No themes found\nA+B=back");
        return;
    }

    // drop entries with suspicious names (path traversal, wrong extension, ...)
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (!safe_theme_name(l->names[i])) continue;
        if (kept != i) {
            memmove(l->names[kept], l->names[i], sizeof(l->names[kept]));
            memmove(l->urls[kept], l->urls[i], sizeof(l->urls[kept]));
        }
        kept++;
    }
    count = kept;
    if (count == 0) {
        show_and_wait("No valid themes\nA+B=back");
        return;
    }

    mkdir("/littlefs/bios", 0755);

    while (true) {
        const char *items[K85_APPREPO_MAX_ASSETS + 1];
        for (int i = 0; i < count; i++) {
            char full_path[192];
            snprintf(full_path, sizeof(full_path), "/littlefs/bios/%s", l->names[i]);
            struct stat st;
            bool installed = (stat(full_path, &st) == 0 && st.st_size > 0);
            snprintf(l->labeled[i], sizeof(l->labeled[i]), "%s%s", l->names[i], installed ? " [installed]" : "");
            items[i] = l->labeled[i];
        }
        items[count] = "Back";

        int idx = k85_run_list_menu("THEMES (UEFI/BIOS)", items, count + 1, nullptr);
        if (idx < 0 || idx >= count) return;

        char dest[192];
        char tmp[200];
        snprintf(dest, sizeof(dest), "/littlefs/bios/%s", l->names[idx]);
        snprintf(tmp, sizeof(tmp), "%s.tmp", dest);

        k85_show_message("Downloading...");

        // download to a temp file, validate, then move into place,
        // so an interrupted download never looks like an installed theme
        unlink(tmp);
        bool ok = k85_apprepo_download_file(l->urls[idx], tmp);
        if (ok) {
            struct stat st;
            ok = (stat(tmp, &st) == 0 && st.st_size > 0 && st.st_size <= K85_THEME_MAX_BYTES);
        }
        if (ok) {
            unlink(dest);
            ok = (rename(tmp, dest) == 0);
        }
        if (!ok) unlink(tmp);

        show_and_wait(ok ? "Downloaded!\nApply via BIOS ->\nUpdate UEFI theme\nA+B=back"
                         : "Download failed\nA+B=back");
    }
}

// ---------- Firmware ----------
static void run_firmware_category(void) {
    if (!require_wifi()) return;

    auto l = psram_alloc<FwList>();
    if (!l) { show_and_wait("Out of memory\nA+B=back"); return; }

    k85_show_message("Checking latest\nfirmware release...");
    int count = 0;
    if (!k85_fwflash_list_available(l->names, l->urls, l->sigs, K85_FW_LIST_MAX, &count) || count == 0) {
        show_and_wait("No firmware found\nA+B=back");
        return;
    }

    while (true) {
        char header[80];
        snprintf(header, sizeof(header), "FIRMWARE (running v%s)", K85_FW_VERSION);

        const char *items[K85_FW_LIST_MAX + 1];
        for (int i = 0; i < count; i++) items[i] = l->names[i];
        items[count] = "Back";

        int idx = k85_run_list_menu(header, items, count + 1, nullptr);
        if (idx < 0 || idx >= count) return;

        flash_selected(l->names[idx], l->urls[idx], l->sigs[idx]);
    }
}

void k85_run_package_manager(void) {
    static const char *items[] = { "Games", "Themes", "Firmware", "Back" };
    while (true) {
        int idx = k85_run_list_menu("PACKAGE MANAGER", items, 4, nullptr);
        if (idx < 0 || idx == 3) return;
        if (idx == 0) run_games_category();
        else if (idx == 1) run_themes_category();
        else if (idx == 2) run_firmware_category();
    }
}
