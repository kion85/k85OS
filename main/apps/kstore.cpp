// K85STORE1: KStore. Lists github.com/kion85/apps_k85os (GitHub contents API) and installs files:
//   .bin  -> free OTA slot (signed when <name>.sig exists, otherwise UNSIGNED after confirmation), never activated
//   .json -> /littlefs/games_json, .thm -> /littlefs/themes, anything else -> /littlefs/store
// A = scroll, B = select, double B / Back = one level up (repo list -> main menu).
#include "kstore.h"
#include "wifi.h"
#include "config.h"
#include "common.h"
#include "list_menu.h"
#include "input.h"
#include "heavy_lock.h"
#include "fs_guard.h"
#include "theme.h"
#include "log.h"
#include "boot_request.h"
#include "app_repo.h"
#include "firmware_flash.h"
#include "json_interpreter.h"
#include "js_engine.h"

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>

#define KS_OWNER "kion85"
#define KS_REPO  "apps_k85os"
#define KS_MAX   48

struct KsEntry { char name[48]; char path[112]; char url[200]; uint32_t size; bool dir; };

struct KsBuf { char *d; size_t len, cap; bool over; };

static int s_ks_last = -1;
static char s_ks_err[96] = "";

static esp_err_t ks_evt(esp_http_client_event_t *e) {
    KsBuf *b = (KsBuf *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->len + e->data_len < b->cap) {
            memcpy(b->d + b->len, e->data, e->data_len);
            b->len += e->data_len;
            b->d[b->len] = 0;
        } else {
            b->over = true;
        }
    }
    return ESP_OK;
}

static void ks_wait_ab(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void ks_msg_wait(const char *m) {
    k85_show_message(m);
    ks_wait_ab();
}

static bool ks_wifi_ready(void) {
    if (k85_wifi_is_connected()) return true;
    k85_show_message("Connecting\nto WiFi...");
    if (k85_wifi_connect_saved()) return true;
    k85_show_message("No WiFi\nSetup it first\n(WiFi Manager)");
    vTaskDelay(pdMS_TO_TICKS(2000));
    return false;
}

static void ks_encode_path(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        if (*in == ' ') { out[o++] = '%'; out[o++] = '2'; out[o++] = '0'; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

// 0 = failed, 1 = ok, 2 = too many entries for the buffer
static int ks_fetch_dir(const char *path, KsEntry *out, int *count) {
    *count = 0;
    char enc[160];
    ks_encode_path(path, enc, sizeof(enc));
    char url[300];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/contents/%s", KS_OWNER, KS_REPO, enc);

    const size_t cap = 120 * 1024;
    char *buf = (char *)heap_caps_calloc(1, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return 0;
    KsBuf b = { buf, 0, cap - 1, false };

    int status = 0;
    esp_err_t err = ESP_FAIL;
    {
        K85HeavyLockGuard hl(20000);
        if (!hl.held) { snprintf(s_ks_err, sizeof(s_ks_err), "busy (another download)"); free(buf); return 0; }
        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.event_handler = ks_evt;
        cfg.user_data = &b;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.buffer_size_tx = 2048;
        cfg.timeout_ms = 12000;
        cfg.user_agent = "k85OS-device";
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) { snprintf(s_ks_err, sizeof(s_ks_err), "http init failed, RAM %uK", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024)); free(buf); return 0; }
        esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
        err = esp_http_client_perform(c);
        status = esp_http_client_get_status_code(c);
        esp_http_client_cleanup(c);
    }
    if (err != ESP_OK || status != 200) {
        if (err != ESP_OK) snprintf(s_ks_err, sizeof(s_ks_err), "%s, RAM %uK/%uK", esp_err_to_name(err), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        else if (status == 403) snprintf(s_ks_err, sizeof(s_ks_err), "HTTP 403 (GitHub limit?)");
        else snprintf(s_ks_err, sizeof(s_ks_err), "HTTP %d", status);
        printf("[kstore] %s\n", s_ks_err);
        free(buf); return 0; }
    if (b.over) { free(buf); return 2; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return 0; }
    int total = cJSON_GetArraySize(root);
    int k = 0;
    for (int i = 0; i < total && k < KS_MAX; i++) {
        cJSON *it = cJSON_GetArrayItem(root, i);
        cJSON *nm = cJSON_GetObjectItem(it, "name");
        cJSON *pa = cJSON_GetObjectItem(it, "path");
        cJSON *ty = cJSON_GetObjectItem(it, "type");
        cJSON *sz = cJSON_GetObjectItem(it, "size");
        cJSON *du = cJSON_GetObjectItem(it, "download_url");
        if (!cJSON_IsString(nm) || !cJSON_IsString(pa) || !cJSON_IsString(ty)) continue;
        KsEntry &e = out[k];
        memset(&e, 0, sizeof(e));
        snprintf(e.name, sizeof(e.name), "%.47s", nm->valuestring);
        snprintf(e.path, sizeof(e.path), "%.111s", pa->valuestring);
        e.dir = !strcmp(ty->valuestring, "dir");
        e.size = cJSON_IsNumber(sz) ? (uint32_t)sz->valuedouble : 0;
        if (cJSON_IsString(du)) snprintf(e.url, sizeof(e.url), "%.199s", du->valuestring);
        if (!e.dir && !e.url[0]) continue;
        k++;
    }
    cJSON_Delete(root);
    *count = k;
    return 1;
}

static const char *ks_ext(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d : "";
}

static void ks_progress(int pct) {
    if (pct / 5 == s_ks_last) return;
    s_ks_last = pct / 5;
    char m[40];
    snprintf(m, sizeof(m), "Flashing...\n%d%%", pct);
    k85_show_message(m);
}

static void ks_install_bin(KsEntry *ents, int n, int idx) {
    KsEntry &e = ents[idx];
    char signame[64];
    snprintf(signame, sizeof(signame), "%.58s.sig", e.name);
    const char *sig_url = nullptr;
    for (int i = 0; i < n; i++) {
        if (!ents[i].dir && !strcmp(ents[i].name, signame)) { sig_url = ents[i].url; break; }
    }
    char head[48];
    snprintf(head, sizeof(head), sig_url ? "%.14s (signed)" : "%.12s UNSIGNED", e.name);
    const char *yn[] = { "Flash to free slot", "Cancel" };
    if (k85_run_list_menu(head, yn, 2, nullptr) != 0) return;
    if (!ks_wifi_ready()) return;

    char msg[120];
    snprintf(msg, sizeof(msg), "Flashing to slot\n%s", k85_fwflash_free_slot_label());
    k85_show_message(msg);
    s_ks_last = -1;
    char info[96] = "";
    bool ok;
    if (sig_url) ok = k85_fwflash_from_url(e.url, sig_url, ks_progress);
    else ok = k85_fwflash_install_thirdparty(e.url, ks_progress, info, sizeof(info));

    if (ok) {
        snprintf(g_config.alt_slot_name, sizeof(g_config.alt_slot_name), "%.60s", e.name);
        k85_config_save();
        k85_boot_request_post(K85_BOOT_REQ_MENU_ALT);
        snprintf(msg, sizeof(msg), "Done: slot %s\nNot activated.\nReboot, then pick\nAlt FW in boot menu\nA+B=back", k85_fwflash_free_slot_label());
        ks_msg_wait(msg);
    } else {
        snprintf(msg, sizeof(msg), "Flash failed\n%.40s\nA+B=back", info[0] ? info : (sig_url ? "bad signature or network" : "network error"));
        ks_msg_wait(msg);
    }
}

static bool ks_save(const KsEntry &e, const char *dir, char *dest, size_t dn) {
    if (strstr(e.name, "..") || strchr(e.name, '/') || strchr(e.name, '\\')) return false;
    char dpath[64];
    snprintf(dpath, sizeof(dpath), "/littlefs/%s", dir);
    mkdir(dpath, 0755);
    snprintf(dest, dn, "%s/%s", dpath, e.name);
    if (k85_is_blocked_path(dest)) return false;
    return k85_apprepo_download_file(e.url, dest);
}

// K85SEC9: a downloaded .lua script runs without a warning only when its Ed25519 signature verifies
// (<name>.lua.sig next to it in the repository, same key as the firmware, 128 hex characters).
#include "ota_verify.h"
#include <sys/stat.h>
#include "esp_heap_caps.h"
enum { KS_SIG_NONE = 0, KS_SIG_OK = 1, KS_SIG_BAD = 2 };
static int ks_check_script_sig(KsEntry *ents, int n, const KsEntry &e, const char *dest) {
    char signame[64];
    snprintf(signame, sizeof(signame), "%.58s.sig", e.name);
    const char *sig_url = nullptr;
    for (int i = 0; i < n; i++) {
        if (!ents[i].dir && !strcmp(ents[i].name, signame)) { sig_url = ents[i].url; break; }
    }
    if (!sig_url) return KS_SIG_NONE;
    char spath[230];
    snprintf(spath, sizeof(spath), "%s.sig", dest);
    if (k85_is_blocked_path(spath)) return KS_SIG_BAD;
    int res = KS_SIG_BAD;
    if (k85_apprepo_download_file(sig_url, spath)) {
        char hex[200] = "";
        FILE *sf = fopen(spath, "rb");
        if (sf) {
            const size_t r = fread(hex, 1, sizeof(hex) - 1, sf);
            hex[r] = 0;
            fclose(sf);
        }
        size_t hl = strlen(hex);
        while (hl && (hex[hl - 1] == '\n' || hex[hl - 1] == '\r' || hex[hl - 1] == ' ')) hex[--hl] = 0;
        struct stat st;
        if (hl == 128 && stat(dest, &st) == 0 && st.st_size > 0 && st.st_size <= 131072) {
            const size_t sz = (size_t)st.st_size;
            uint8_t *buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) buf = (uint8_t *)malloc(sz);
            if (buf) {
                FILE *f = fopen(dest, "rb");
                const size_t rd = f ? fread(buf, 1, sz, f) : 0;
                if (f) fclose(f);
                if (rd == sz && k85_ota_verify_signature_hex(buf, sz, hex)) res = KS_SIG_OK;
                free(buf);
            }
        }
    }
    remove(spath);
    return res;
}

static void ks_handle_file(KsEntry *ents, int n, int idx) {
    KsEntry &e = ents[idx];
    const char *x = ks_ext(e.name);
    if (!strcasecmp(x, ".bin")) { ks_install_bin(ents, n, idx); return; }
    if (!ks_wifi_ready()) return;

    const char *dir = "store";
    if (!strcasecmp(x, ".json")) dir = "games_json";
    else if (!strcasecmp(x, ".thm")) dir = "themes";  else if (!strcasecmp(x, ".lua")) dir = "scripts";

    k85_show_message("Downloading...");
    char dest[200];
    if (!ks_save(e, dir, dest, sizeof(dest))) { ks_msg_wait("Download failed\nA+B=back"); return; }

    char msg[120];
    if (!strcasecmp(x, ".json")) {
        const char *yn[] = { "Run now", "Later" };
        if (k85_run_list_menu("Game saved", yn, 2, nullptr) == 0) k85_json_run_file(dest);
    } else if (!strcasecmp(x, ".lua")) {
        const int k85_sg = ks_check_script_sig(ents, n, e, dest);   // K85SEC9
        if (k85_sg == KS_SIG_BAD) { remove(dest); ks_msg_wait("Bad signature!\nScript removed\nA+B=back"); return; }
        const char *yn2[] = { k85_sg == KS_SIG_OK ? "Run now (signed)" : "Keep, do not run", k85_sg == KS_SIG_OK ? "Later" : "Run UNSIGNED" };
        if (k85_run_list_menu(k85_sg == KS_SIG_OK ? "Script saved (signed)" : "Script UNSIGNED", yn2, 2, nullptr) == (k85_sg == KS_SIG_OK ? 0 : 1)) {
            static EXT_RAM_BSS_ATTR char jout[1024];
            bool jok = k85_js_run_file(dest, jout, sizeof(jout), true);
            char jm[260];
            snprintf(jm, sizeof(jm), "%.190s\nA+B=back", jout[0] ? jout : (jok ? "(done)" : "(failed)"));
            ks_msg_wait(jm);
        }
    } else if (!strcasecmp(x, ".thm")) {
        k85_themes_load_custom();
        ks_msg_wait("Theme installed\nSettings -> Theme\nA+B=back");
    } else {
        snprintf(msg, sizeof(msg), "Saved to\n%.40s\nA+B=back", dest + 9);
        ks_msg_wait(msg);
    }
}

static void ks_browse(KsEntry *ents) {
    static EXT_RAM_BSS_ATTR char labels[KS_MAX + 3][36];
    const char *items[KS_MAX + 3];
    int map[KS_MAX + 3];
    char path[112] = "";
    char stack[6][112];
    int depth = 0;
    int n = 0;
    bool main_page = false;
    bool need_fetch = true;

    while (true) {
        if (need_fetch) {
            k85_show_message("Loading...");
            int r = ks_fetch_dir(path, ents, &n);
            if (r != 1) {
                { char em[160]; if (r == 2) snprintf(em, sizeof(em), "Too many files\nin this folder\nA+B=back"); else snprintf(em, sizeof(em), "Load failed\n%.90s\nA+B=back", s_ks_err); ks_msg_wait(em); }
                if (depth == 0) return;
                snprintf(path, sizeof(path), "%s", stack[--depth]);
                continue;
            }
            need_fetch = false;
        }

        int cnt = 0;
        bool root = (path[0] == 0);
        if (root && !main_page) {
            snprintf(labels[cnt], sizeof(labels[0]), "[Main page]");
            items[cnt] = labels[cnt]; map[cnt] = -2; cnt++;
        }
        for (int i = 0; i < n && cnt < KS_MAX + 2; i++) {
            const KsEntry &e = ents[i];
            if (e.name[0] == '.') continue;
            if (!e.dir && !strcasecmp(ks_ext(e.name), ".sig")) continue;
            if (root && !main_page && !e.dir) continue;
            if (root && main_page && e.dir) continue;
            if (e.dir) snprintf(labels[cnt], sizeof(labels[0]), "%.18s/", e.name);
            else snprintf(labels[cnt], sizeof(labels[0]), "%.19s", e.name);
            items[cnt] = labels[cnt]; map[cnt] = i; cnt++;
        }
        items[cnt] = "Back"; map[cnt] = -1; cnt++;

        const char *title = "KSTORE";
        if (!root) { const char *s = strrchr(path, '/'); title = s ? s + 1 : path; }
        else if (main_page) title = "KSTORE main";

        int sel = k85_run_list_menu(title, items, cnt, nullptr);
        int m = (sel < 0 || sel >= cnt) ? -1 : map[sel];

        if (m == -2) { main_page = true; continue; }
        if (m == -1) {
            if (main_page) { main_page = false; continue; }
            if (depth > 0) { snprintf(path, sizeof(path), "%s", stack[--depth]); need_fetch = true; continue; }
            return;
        }
        if (ents[m].dir) {
            if (depth >= 6) continue;
            snprintf(stack[depth++], sizeof(stack[0]), "%s", path);
            snprintf(path, sizeof(path), "%s", ents[m].path);
            need_fetch = true;
            continue;
        }
        ks_handle_file(ents, n, m);
    }
}

void k85_run_kstore(void) {
    if (!ks_wifi_ready()) return;
    KsEntry *ents = (KsEntry *)heap_caps_calloc(KS_MAX, sizeof(KsEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ents) { k85_show_message("Out of memory"); vTaskDelay(pdMS_TO_TICKS(1500)); return; }
    ks_browse(ents);
    free(ents);
}