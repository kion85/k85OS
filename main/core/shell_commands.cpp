#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "shell_commands.h"
#include "config.h"
#include "device.h"
#include "battery.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "M5Unified.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static void cmd_info(char *out, size_t out_size) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_sz = 0;
    esp_flash_get_size(nullptr, &flash_sz);
    size_t psram_sz = esp_psram_is_initialized() ? esp_psram_get_size() : 0;

    snprintf(out, out_size,
        "Chip: %s rev v%d.%d, %d cores\r\n"
        "Flash: %lu MB, PSRAM: %lu MB\r\n"
        "Device name: %s\r\n",
        CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100, chip.cores,
        (unsigned long)(flash_sz / (1024 * 1024)), (unsigned long)(psram_sz / (1024 * 1024)),
        k85_get_device_name());
}

static void cmd_battery(char *out, size_t out_size) {
    int pct = k85_get_battery();
    snprintf(out, out_size, "Battery: %s%s\r\n",
             pct >= 0 ? (std::to_string(pct) + "%").c_str() : "unknown",
             k85_is_charging() ? " (charging)" : "");
}

static void cmd_imu(char *out, size_t out_size) {
    float ax, ay, az, gx, gy, gz;
    M5.Imu.update();
    auto data = M5.Imu.getImuData();
    ax = data.accel.x; ay = data.accel.y; az = data.accel.z;
    gx = data.gyro.x; gy = data.gyro.y; gz = data.gyro.z;
    snprintf(out, out_size,
        "Accel: %.2f %.2f %.2f\r\nGyro: %.2f %.2f %.2f\r\n",
        ax, ay, az, gx, gy, gz);
}

static void cmd_ls(const char *arg, char *out, size_t out_size) {
    char path[192];
    snprintf(path, sizeof(path), "/littlefs%s%s", (arg[0] && arg[0] != '/') ? "/" : "", arg);
    DIR *d = opendir(path[9] ? path : "/littlefs");
    if (!d) { snprintf(out, out_size, "ls: cannot open %s\r\n", path); return; }

    size_t used = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr && used < out_size - 64) {
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        bool is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        int w = snprintf(out + used, out_size - used, "%s%s\r\n", ent->d_name, is_dir ? "/" : "");
        if (w > 0) used += w;
    }
    closedir(d);
    if (used == 0) snprintf(out, out_size, "(empty)\r\n");
}

static void cmd_cat(const char *arg, char *out, size_t out_size) {
    char path[192];
    snprintf(path, sizeof(path), "/littlefs/%s", arg);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(out, out_size, "cat: cannot open %s\r\n", arg); return; }
    size_t r = fread(out, 1, out_size - 3, f);
    fclose(f);
    out[r] = 0;
    strcat(out, "\r\n");
}

void k85_shell_run_command(const char *cmd_full, char *out, size_t out_size) {
    char cmd[32] = {0};
    const char *arg = "";
    const char *sp = strchr(cmd_full, ' ');
    if (sp) {
        size_t clen = sp - cmd_full;
        if (clen >= sizeof(cmd)) clen = sizeof(cmd) - 1;
        memcpy(cmd, cmd_full, clen);
        arg = sp + 1;
    } else {
        snprintf(cmd, sizeof(cmd), "%s", cmd_full);
    }

    if (!strcmp(cmd, "help")) {
        snprintf(out, out_size,
            "Commands:\r\n"
            "  info, free, uptime, battery, wifi, imu\r\n"
            "  ls [dir], cat <file>\r\n"
            "  wifi-on, wifi-off, brightness <0-100>, volume <0-100>\r\n"
            "  reboot, exit\r\n");
    } else if (!strcmp(cmd, "info")) {
        cmd_info(out, out_size);
    } else if (!strcmp(cmd, "free")) {
        snprintf(out, out_size, "Free heap: %lu KB\r\n", (unsigned long)(esp_get_free_heap_size() / 1024));
    } else if (!strcmp(cmd, "uptime")) {
        int64_t us = esp_timer_get_time();
        snprintf(out, out_size, "Uptime: %lld sec\r\n", (long long)(us / 1000000));
    } else if (!strcmp(cmd, "battery")) {
        cmd_battery(out, out_size);
    } else if (!strcmp(cmd, "wifi")) {
        snprintf(out, out_size, "WiFi SSID: %s\r\n", g_config.wifi_ssid[0] ? g_config.wifi_ssid : "(none)");
    } else if (!strcmp(cmd, "imu")) {
        cmd_imu(out, out_size);
    } else if (!strcmp(cmd, "ls")) {
        cmd_ls(arg, out, out_size);
    } else if (!strcmp(cmd, "cat")) {
        cmd_cat(arg, out, out_size);
    } else if (!strcmp(cmd, "wifi-on")) {
        g_config.wifi_disabled = false;
        k85_config_save();
        snprintf(out, out_size, "WiFi enabled\r\n");
    } else if (!strcmp(cmd, "wifi-off")) {
        g_config.wifi_disabled = true;
        k85_config_save();
        snprintf(out, out_size, "WiFi disabled\r\n");
    } else if (!strcmp(cmd, "brightness")) {
        int v = atoi(arg);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        g_config.brightness_active = v;
        M5.Display.setBrightness(v);
        k85_config_save();
        snprintf(out, out_size, "Brightness set to %d%%\r\n", v);
    } else if (!strcmp(cmd, "volume")) {
        int v = atoi(arg);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        g_config.sound_volume = v;
        k85_config_save();
        snprintf(out, out_size, "Volume set to %d%%\r\n", v);
    } else if (!strcmp(cmd, "reboot")) {
        snprintf(out, out_size, "Rebooting...\r\n");
    } else if (strlen(cmd) == 0) {
        out[0] = 0;
    } else {
        snprintf(out, out_size, "Unknown command: %s (try 'help')\r\n", cmd);
    }
}

#pragma GCC diagnostic pop