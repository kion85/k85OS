#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "bios_extras.h"
#include "bios_diag.h"
#include "bios_password.h"
#include "bios_system.h"
#include "common.h"
#include "input.h"
#include "config.h"
#include "wifi.h"
#include "battery.h"
#include "list_menu.h"
#include "../core/bios_theme.h"
#include "../core/svc_cfg.h"
#include "../core/cron.h"
#include "../net/ftp_server.h"
#include "../net/ota_verify.h"
#include "mbedtls/sha256.h"

#include "M5Unified.h"
#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#define K85_CFG_BACKUP     "/littlefs/k85os_config.json.bak"
#define K85_CFG_BACKUP_TMP "/littlefs/k85os_config.json.bak.tmp"
#define K85_CFG_RESTORE_TMP "/littlefs/k85os_config.json.tmp"

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

static bool confirm_action(const char *label) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s?\nB=confirm A+B=cancel", label);
    k85_show_message(msg);
    k85_wait_ab_release();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) return true;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Hardware monitor ----------
static void hardware_monitor(void) {
    K85BiosTheme th = k85_bios_theme_load();
    uint32_t bg = (g_config.bios_bg_color != 0xFFFFFFFF) ? g_config.bios_bg_color : th.bg;
    uint32_t fg = (g_config.bios_text_color != 0xFFFFFFFF) ? g_config.bios_text_color : th.fg;
    uint32_t accent = (g_config.bios_hl_color != 0xFFFFFFFF) ? g_config.bios_hl_color : th.accent;

    auto &d = M5.Display;
    d.fillScreen(bg);
    d.setTextSize(1);
    d.setTextColor(accent, bg);
    d.setCursor(6, 4);
    d.print("HARDWARE MONITOR");
    d.drawFastHLine(2, 14, d.width() - 4, accent);
    d.setCursor(6, d.height() - 12);
    d.print("A+B=exit");

    temperature_sensor_handle_t ts = nullptr;
    bool ts_ok = false;
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &ts) == ESP_OK) {
        if (temperature_sensor_enable(ts) == ESP_OK) {
            ts_ok = true;
        } else {
            temperature_sensor_uninstall(ts);
            ts = nullptr;
        }
    }

    int64_t last = 0;
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }

        int64_t now = esp_timer_get_time();
        if (last == 0 || now - last >= 1000000) {
            last = now;
            char line[64], pad[64], temp[16];
            int row = 0;
            auto put = [&](const char *txt) {
                snprintf(pad, sizeof(pad), "%-38s", txt);
                d.setTextColor(fg, bg);
                d.setCursor(6, 20 + row * 13);
                d.print(pad);
                row++;
            };

            float c = 0;
            if (ts_ok && temperature_sensor_get_celsius(ts, &c) == ESP_OK) snprintf(temp, sizeof(temp), "%.1f C", c);
            else snprintf(temp, sizeof(temp), "N/A");
            snprintf(line, sizeof(line), "CPU %u MHz   Temp %s", (unsigned)esp_rom_get_cpu_ticks_per_us(), temp);
            put(line);

            snprintf(line, sizeof(line), "RAM %u/%u KB free",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024));
            put(line);
            snprintf(line, sizeof(line), "Largest RAM block %u KB",
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
            put(line);

            size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
            if (ps_total) {
                snprintf(line, sizeof(line), "PSRAM %u/%u KB free",
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (unsigned)(ps_total / 1024));
            } else {
                snprintf(line, sizeof(line), "PSRAM: none");
            }
            put(line);

            int pct = k85_get_battery();
            int32_t mv = M5.Power.getBatteryVoltage();
            if (pct >= 0 && mv > 0) snprintf(line, sizeof(line), "Battery %d%%  %.2f V", pct, mv / 1000.0f);
            else if (pct >= 0) snprintf(line, sizeof(line), "Battery %d%%", pct);
            else snprintf(line, sizeof(line), "Battery: N/A");
            put(line);

            if (k85_wifi_is_connected()) snprintf(line, sizeof(line), "WiFi %d dBm", k85_wifi_get_rssi());
            else snprintf(line, sizeof(line), "WiFi: not connected");
            put(line);

            int64_t s = now / 1000000;
            snprintf(line, sizeof(line), "Uptime %02d:%02d:%02d", (int)(s / 3600), (int)((s % 3600) / 60), (int)(s % 60));
            put(line);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    if (ts) {
        temperature_sensor_disable(ts);
        temperature_sensor_uninstall(ts);
    }
}

// ---------- Safe mode / automation ----------
static void automation_menu(void) {
    while (true) {
        bool st = k85_svc_get_bool("startup", true);
        bool cr = k85_svc_get_bool("cron", false);
        bool fa = k85_svc_get_bool("ftp_auto", false);
        char l0[40], l1[40], l2[40];
        snprintf(l0, sizeof(l0), "Startup script: %s", st ? "ON" : "OFF");
        snprintf(l1, sizeof(l1), "Scheduler: %s", cr ? "ON" : "OFF");
        snprintf(l2, sizeof(l2), "FTP autostart: %s", fa ? "ON" : "OFF");

        const char *items[] = { l0, l1, l2, "Disable all autostart", "Back" };
        int idx = k85_run_list_menu("AUTOMATION", items, 5, nullptr);
        if (idx < 0 || idx == 4) return;

        if (idx == 0) {
            k85_svc_set_bool("startup", !st);
        } else if (idx == 1) {
            k85_svc_set_bool("cron", !cr);
            if (cr) k85_cron_stop(); else k85_cron_start();
        } else if (idx == 2) {
            k85_svc_set_bool("ftp_auto", !fa);
        } else if (idx == 3) {
            if (confirm_action("Disable all autostart")) {
                k85_svc_set_bool("startup", false);
                k85_svc_set_bool("cron", false);
                k85_svc_set_bool("ftp_auto", false);
                k85_cron_stop();
                k85_ftp_server_stop();
                show_and_wait("Autostart disabled\nA+B=back");
            }
        }
    }
}

// ---------- Config backup / restore ----------
static bool copy_file(const char *src, const char *dst_tmp, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst_tmp, "wb");
    if (!out) { fclose(in); return false; }
    char buf[512];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (ok) {
        unlink(dst);
        ok = (rename(dst_tmp, dst) == 0);
    }
    if (!ok) unlink(dst_tmp);
    return ok;
}

static void backup_config(void) {
    k85_config_save();   // make sure the file is current
    bool ok = copy_file(CONFIG_FILE_PATH, K85_CFG_BACKUP_TMP, K85_CFG_BACKUP);
    show_and_wait(ok ? "Config backed up\n(stays on device)\nA+B=back" : "Backup failed\nA+B=back");
}

static void restore_config(void) {
    struct stat st;
    if (stat(K85_CFG_BACKUP, &st) != 0) {
        show_and_wait("No backup found\nA+B=back");
        return;
    }
    if (!confirm_action("Restore config backup")) return;
    if (!copy_file(K85_CFG_BACKUP, K85_CFG_RESTORE_TMP, CONFIG_FILE_PATH)) {
        show_and_wait("Restore failed\nA+B=back");
        return;
    }
    k85_show_message("Config restored\nRebooting...");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

// ---------- Signature info ----------
static void signature_info(void) {
    uint8_t key[32];
    k85_ota_get_public_key(key);
    uint8_t h[32];
    if (mbedtls_sha256(key, sizeof(key), h, 0) != 0) {
        show_and_wait("Hash failed\nA+B=back");
        return;
    }
    char l1[17], l2[17];
    for (int i = 0; i < 8; i++) {
        snprintf(l1 + 2 * i, 3, "%02X", h[i]);
        snprintf(l2 + 2 * i, 3, "%02X", h[8 + i]);
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "Firmware signature:\nEd25519, required\nKey SHA-256:\n%s\n%s\nA+B=back", l1, l2);
    show_and_wait(msg);
}

// ---------- entry ----------
void k85_run_bios_extras(void) {
    static const char *items[] = {
        "Hardware monitor", "Automation / safe", "Backup config", "Restore config", "Signature info", "Diagnostics", "Slots & partitions", "Security status", "BIOS password", "Restore points", "Integrity check", "Back",
    };
    while (true) {
        int idx = k85_run_list_menu("SYSTEM TOOLS", items, 12, nullptr);
        if (idx < 0 || idx == 11) return;
        if (idx == 0) hardware_monitor();
        else if (idx == 1) automation_menu();
        else if (idx == 2) backup_config();
        else if (idx == 3) restore_config();
        else if (idx == 4) signature_info();
        else if (idx == 5) k85_run_diagnostics();
        else if (idx == 6) k85_run_slots_menu();
        else if (idx == 7) k85_run_security_status();
        else if (idx == 8) k85_run_bios_password_menu();
        else if (idx == 9) k85_run_restore_points();
        else if (idx == 10) k85_run_integrity_check();
    }
}

#pragma GCC diagnostic pop