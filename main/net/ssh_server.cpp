#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "ssh_server.h"
#include "core/shell_commands.h"
#include "core/config.h"
#include "core/log.h"
#include "core/battery.h"
#include "core/device.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "M5Unified.h"
#include <dirent.h>
#include <sys/stat.h>

#include "wolfssh/ssh.h"
#include "wolfssl/wolfcrypt/ecc.h"
#include "wolfssl/wolfcrypt/asn_public.h"
#include "wolfssl/wolfcrypt/random.h"
#include "mbedtls/sha256.h"

#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "k85_ssh";
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#define K85_SSH_KEY_PATH "/littlefs/k85_ssh_host_key.der"
#define K85_SSH_PORT 22

static TaskHandle_t s_ssh_task = nullptr; // task self-cleanup (static stack stays allocated for reuse)
static volatile bool s_ssh_running = false;
static WOLFSSH_CTX *s_ctx = nullptr;

static StaticTask_t s_ssh_task_buf;
static StackType_t *s_ssh_task_stack = nullptr;
// ВАЖНО: на ESP-IDF FreeRTOS-порте StackType_t == uint8_t, а usStackDepth
// в xTaskCreate*/heap_caps_malloc задаётся в БАЙТАХ (не в словах, в отличие
// от vanilla FreeRTOS!). Раньше здесь было 6144 * sizeof(StackType_t), что
// при sizeof(StackType_t)==1 давало РЕАЛЬНЫЙ стек всего 6144 байт вместо
// задуманных 24576 — отсюда и просадка после перехода на раннюю резервацию.
static_assert(sizeof(StackType_t) == 1,
    "StackType_t не uint8_t на этой платформе — пересчитай K85_SSH_STACK_BYTES!");
#define K85_SSH_STACK_BYTES 12288  // 12 KB с запасом под fp_int-цепочку wolfCrypt

// (constructor убран, вызывается явно из app_main)
void k85_ssh_reserve_stack_early(void) {
    s_ssh_task_stack = (StackType_t *)heap_caps_malloc(
        K85_SSH_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// ---------- ????-???? (ECDSA P-256), ???????????? ???? ???, ???????? ? LittleFS ----------
static bool load_or_generate_host_key(byte **out_der, word32 *out_der_sz) {
    FILE *f = fopen(K85_SSH_KEY_PATH, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 512) {
            byte *buf = (byte *)malloc(sz);
            if (buf && fread(buf, 1, sz, f) == (size_t)sz) {
                fclose(f);
                *out_der = buf;
                *out_der_sz = (word32)sz;
                return true;
            }
            if (buf) free(buf);
        }
        fclose(f);
    }

    WC_RNG rng;
    if (wc_InitRng(&rng) != 0) return false;

    ecc_key key;
    wc_ecc_init(&key);
    int ret = wc_ecc_make_key(&rng, 32, &key); // P-256
    if (ret != 0) {
        wc_ecc_free(&key);
        wc_FreeRng(&rng);
        k85_log("ssh: host key generation failed: %d", ret);
        return false;
    }

    byte der[256];
    int der_sz = wc_EccKeyToDer(&key, der, sizeof(der));
    wc_ecc_free(&key);
    wc_FreeRng(&rng);

    if (der_sz <= 0) {
        k85_log("ssh: host key DER encoding failed: %d", der_sz);
        return false;
    }

    FILE *wf = fopen(K85_SSH_KEY_PATH, "wb");
    if (wf) {
        fwrite(der, 1, der_sz, wf);
        fclose(wf);
    }

    byte *buf = (byte *)malloc(der_sz);
    if (!buf) return false;
    memcpy(buf, der, der_sz);
    *out_der = buf;
    *out_der_sz = (word32)der_sz;
    k85_log("ssh: generated new ECDSA host key (%d bytes)", der_sz);
    return true;
}

// ---------- ?????? ----------
void k85_ssh_hash_password(const char *password, char *out_hex) {
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char *)password, strlen(password), hash, 0);
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", hash[i]);
}

static void sha256_hex(const char *input, char *out_hex) {
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char *)input, strlen(input), hash, 0);
    for (int i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", hash[i]);
}

// ---------- ?????????????? ----------
static int ssh_user_auth_cb(byte authType, WS_UserAuthData *authData, void *ctx) {
    (void)ctx;
    if (authType != WOLFSSH_USERAUTH_PASSWORD) return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;

    if (!g_config.ssh_enabled || g_config.ssh_username[0] == 0 || g_config.ssh_password_hash[0] == 0) {
        return WOLFSSH_USERAUTH_REJECTED;
    }

    if (authData->usernameSz != strlen(g_config.ssh_username) ||
        memcmp(authData->username, g_config.ssh_username, authData->usernameSz) != 0) {
        return WOLFSSH_USERAUTH_INVALID_USER;
    }

    char pass_buf[128] = {0};
    word32 plen = authData->sf.password.passwordSz;
    if (plen >= sizeof(pass_buf)) plen = sizeof(pass_buf) - 1;
    memcpy(pass_buf, authData->sf.password.password, plen);

    char hash_hex[65];
    sha256_hex(pass_buf, hash_hex);

    if (strcasecmp(hash_hex, g_config.ssh_password_hash) != 0) {
        return WOLFSSH_USERAUTH_INVALID_PASSWORD;
    }

    return WOLFSSH_USERAUTH_SUCCESS;
}

// ---------- ????????? ????????????? ?????? ??? ?????? ----------
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

static void build_command_response(const char *cmd_full, char *out, size_t out_size) {
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

static void handle_ssh_session(WOLFSSH *ssh) {
    char line[128];
    int line_len = 0;

    const char *banner = "k85OS SSH terminal. Type 'help' for commands.\r\n> ";
    wolfSSH_stream_send(ssh, (byte *)banner, strlen(banner));

    while (s_ssh_running) {
        byte c;
        int r = wolfSSH_stream_read(ssh, &c, 1);
        if (r <= 0) break;

        if (c == '\r' || c == '\n') {
            line[line_len] = 0;
            wolfSSH_stream_send(ssh, (byte *)"\r\n", 2);

            if (!strcmp(line, "exit")) break;

            char resp[160];
            k85_shell_run_command(line, resp, sizeof(resp));
            if (resp[0]) wolfSSH_stream_send(ssh, (byte *)resp, strlen(resp));

            bool do_reboot = !strcmp(line, "reboot");

            wolfSSH_stream_send(ssh, (byte *)"> ", 2);
            line_len = 0;

            if (do_reboot) {
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else if (c == 127 || c == 8) { // backspace
            if (line_len > 0) {
                line_len--;
                wolfSSH_stream_send(ssh, (byte *)"\b \b", 3);
            }
        } else if (line_len < (int)sizeof(line) - 1) {
            line[line_len++] = (char)c;
            wolfSSH_stream_send(ssh, &c, 1); // echo
        }
    }
}

static void ssh_server_task(void *arg) {
    byte *host_key_der = nullptr;
    word32 host_key_sz = 0;
    if (!load_or_generate_host_key(&host_key_der, &host_key_sz)) {
        k85_log("ssh: failed to load/generate host key, server not starting");
        s_ssh_running = false;
        s_ssh_task = nullptr; // task self-cleanup (static stack stays allocated for reuse)
        vTaskDelete(nullptr);
        return;
    }

    wolfSSH_Init();
    s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, nullptr);
    if (!s_ctx) {
        k85_log("ssh: CTX_new failed");
        free(host_key_der);
        s_ssh_running = false;
        s_ssh_task = nullptr; // task self-cleanup (static stack stays allocated for reuse)
        vTaskDelete(nullptr);
        return;
    }

    wolfSSH_CTX_UsePrivateKey_buffer(s_ctx, host_key_der, host_key_sz, WOLFSSH_FORMAT_ASN1);
    free(host_key_der);

    wolfSSH_SetUserAuth(s_ctx, ssh_user_auth_cb);

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(K85_SSH_PORT);
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 2);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    k85_log("ssh: server listening on port %d", K85_SSH_PORT);

    while (s_ssh_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        struct timeval sel_tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(listen_fd + 1, &fds, nullptr, nullptr, &sel_tv);
        if (sel <= 0 || !FD_ISSET(listen_fd, &fds)) continue;

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd < 0) continue;

        WOLFSSH *ssh = wolfSSH_new(s_ctx);
        if (!ssh) { close(conn_fd); continue; }

        wolfSSH_set_fd(ssh, conn_fd);

        int accept_ret = wolfSSH_accept(ssh);
        if (accept_ret == WS_SUCCESS) {
            k85_log("ssh: client authenticated, session started");
            handle_ssh_session(ssh);
        } else {
            k85_log("ssh: handshake/auth failed, wolfSSH code=%d", accept_ret);
        }

        wolfSSH_stream_exit(ssh, 0);
        wolfSSH_free(ssh);
        close(conn_fd);
    }

    close(listen_fd);
    wolfSSH_CTX_free(s_ctx);
    s_ctx = nullptr;
    wolfSSH_Cleanup();

    s_ssh_task = nullptr; // task self-cleanup (static stack stays allocated for reuse)
    vTaskDelete(nullptr);
}

K85SshStartResult k85_ssh_server_start(void) {
    wolfSSH_Debugging_ON(); // временно, для диагностики "-1001" — снять после отладки
    if (s_ssh_task) {
        k85_log("ssh: start requested but already running/stuck");
        return K85_SSH_START_ALREADY_RUNNING;
    }
    if (!g_config.wifi_saved) {
        k85_log("ssh: no WiFi configured, cannot start");
        return K85_SSH_START_NO_WIFI;
    }

    s_ssh_running = true;
        if (!s_ssh_task_stack) {
        k85_log("ssh: no reserved stack available (early allocation failed)");
        s_ssh_running = false;
        return K85_SSH_START_TASK_FAILED;
    }
    s_ssh_task = xTaskCreateStaticPinnedToCore(
        ssh_server_task, "k85_ssh", K85_SSH_STACK_BYTES, nullptr, 5,
        s_ssh_task_stack, &s_ssh_task_buf, tskNO_AFFINITY);
    BaseType_t ok = (s_ssh_task != nullptr) ? pdPASS : pdFAIL;
    if (ok != pdPASS) {
        k85_log("ssh: xTaskCreate failed (out of internal RAM?)");
        s_ssh_running = false;
        s_ssh_task = nullptr;
        return K85_SSH_START_TASK_FAILED;
    }
    return K85_SSH_START_OK;
}

void k85_ssh_server_stop(void) {
    s_ssh_running = false;
}

bool k85_ssh_server_is_running(void) {
    return s_ssh_task != nullptr;
}

#pragma GCC diagnostic pop











