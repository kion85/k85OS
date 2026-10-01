#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "bios_diag.h"
#include "common.h"
#include "input.h"
#include "config.h"
#include "list_menu.h"
#include "../core/bios_theme.h"
#include "../core/svc_cfg.h"

#include "M5Unified.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_flash_encrypt.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

struct Pal { uint32_t bg, fg, accent; };

static Pal pal(void) {
    K85BiosTheme th = k85_bios_theme_load();
    Pal p;
    p.bg = (g_config.bios_bg_color != 0xFFFFFFFF) ? g_config.bios_bg_color : th.bg;
    p.fg = (g_config.bios_text_color != 0xFFFFFFFF) ? g_config.bios_text_color : th.fg;
    p.accent = (g_config.bios_hl_color != 0xFFFFFFFF) ? g_config.bios_hl_color : th.accent;
    return p;
}

static void draw_frame(const char *title, const Pal &p) {
    auto &d = M5.Display;
    d.fillScreen(p.bg);
    d.setTextSize(1);
    d.setTextColor(p.accent, p.bg);
    d.setCursor(6, 4);
    d.print(title);
    d.drawFastHLine(2, 14, d.width() - 4, p.accent);
}

static void put_row(int row, const char *txt, const Pal &p) {
    char pad[64];
    snprintf(pad, sizeof(pad), "%-38s", txt);
    M5.Display.setTextColor(p.fg, p.bg);
    M5.Display.setCursor(6, 20 + row * 13);
    M5.Display.print(pad);
}

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

static bool confirm_msg(const char *msg) {
    k85_show_message(msg);
    k85_wait_ab_release();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return false; }
        if (k85_btn_b_pressed()) return true;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ======================= Diagnostics =======================
static void test_display(void) {
    auto &d = M5.Display;
    const int w = d.width(), h = d.height();
    const int pages = 7;
    int page = 0;
    bool redraw = true;
    static const uint32_t bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_b_pressed()) { page = (page + 1) % pages; redraw = true; }
        if (redraw) {
            redraw = false;
            switch (page) {
                case 0: d.fillScreen((uint32_t)0xFF0000); break;
                case 1: d.fillScreen((uint32_t)0x00FF00); break;
                case 2: d.fillScreen((uint32_t)0x0000FF); break;
                case 3: d.fillScreen((uint32_t)0xFFFFFF); break;
                case 4: d.fillScreen((uint32_t)0x000000); break;
                case 5:
                    for (int i = 0; i < 8; i++) d.fillRect(i * w / 8, 0, w / 8 + 1, h, bars[i]);
                    break;
                default:
                    for (int x = 0; x < w; x++) {
                        uint32_t v = (uint32_t)(x * 255 / (w - 1));
                        d.drawFastVLine(x, 0, h, (v << 16) | (v << 8) | v);
                    }
                    break;
            }
            d.setTextSize(1);
            d.setTextColor((uint32_t)(page == 3 ? 0x000000 : 0xFFFFFF));
            d.setCursor(4, h - 10);
            d.print("B=next  A+B=exit");
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void test_buttons(void) {
    Pal p = pal();
    draw_frame("BUTTON TEST", p);
    put_row(4, "Hold A+B to exit", p);
    bool prev_a = false, prev_b = false;
    int ca = 0, cb = 0;
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        bool a = M5.BtnA.isPressed();
        bool b = M5.BtnB.isPressed();
        if (a && !prev_a) ca++;
        if (b && !prev_b) cb++;
        prev_a = a;
        prev_b = b;
        char line[48];
        snprintf(line, sizeof(line), "Button A: %-5s  presses: %d", a ? "DOWN" : "up", ca);
        put_row(0, line, p);
        snprintf(line, sizeof(line), "Button B: %-5s  presses: %d", b ? "DOWN" : "up", cb);
        put_row(1, line, p);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void test_speaker(void) {
    if (!M5.Speaker.isEnabled()) { show_and_wait("No speaker found\nA+B=back"); return; }
    if (g_config.sound_muted || g_config.sound_volume <= 0) {
        show_and_wait("Sound is muted or\nvolume is 0\nA+B=back");
        return;
    }
    k85_show_message("Speaker test...\n440 / 880 / 1320 Hz");
    static const int freqs[3] = { 440, 880, 1320 };
    for (int i = 0; i < 3; i++) {
        M5.Speaker.tone(freqs[i], 250);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    show_and_wait("Did you hear\n3 rising tones?\nA+B=back");
}

static void test_imu(void) {
    if (!M5.Imu.isEnabled()) { show_and_wait("IMU not found\nA+B=back"); return; }
    Pal p = pal();
    draw_frame("IMU TEST", p);
    put_row(7, "A+B=exit", p);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        M5.Imu.update();
        auto data = M5.Imu.getImuData();
        char line[48];
        snprintf(line, sizeof(line), "Accel X %7.2f", data.accel.x); put_row(0, line, p);
        snprintf(line, sizeof(line), "Accel Y %7.2f", data.accel.y); put_row(1, line, p);
        snprintf(line, sizeof(line), "Accel Z %7.2f", data.accel.z); put_row(2, line, p);
        snprintf(line, sizeof(line), "Gyro  X %7.2f", data.gyro.x); put_row(3, line, p);
        snprintf(line, sizeof(line), "Gyro  Y %7.2f", data.gyro.y); put_row(4, line, p);
        snprintf(line, sizeof(line), "Gyro  Z %7.2f", data.gyro.z); put_row(5, line, p);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void test_rtc(void) {
    if (!M5.Rtc.isEnabled()) { show_and_wait("RTC not found\nA+B=back"); return; }
    Pal p = pal();
    draw_frame("RTC TEST", p);
    put_row(4, "A+B=exit", p);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        auto dt = M5.Rtc.getDateTime();
        char line[48];
        snprintf(line, sizeof(line), "Date: %04d-%02d-%02d", (int)dt.date.year, (int)dt.date.month, (int)dt.date.date);
        put_row(0, line, p);
        snprintf(line, sizeof(line), "Time: %02d:%02d:%02d", (int)dt.time.hours, (int)dt.time.minutes, (int)dt.time.seconds);
        put_row(1, line, p);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static inline uint32_t psram_pattern(int pass, const volatile uint32_t *a) {
    if (pass == 0) return 0x55AA55AAu;
    if (pass == 1) return 0xAA55AA55u;
    return (uint32_t)(uintptr_t)a ^ 0xA5A5A5A5u;
}

// Tests the currently FREE part of PSRAM (memory in use by the system is left alone).
static void test_psram(void) {
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) { show_and_wait("No PSRAM found\nA+B=back"); return; }
    k85_show_message("PSRAM test...\nplease wait");

    const size_t kChunk = 256 * 1024;
    const size_t kKeep = 512 * 1024;
    uint32_t *chunks[40];
    int n = 0;
    while (n < 40 && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > kKeep + kChunk) {
        chunks[n] = (uint32_t *)heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM);
        if (!chunks[n]) break;
        n++;
    }

    size_t errors = 0;
    uintptr_t first_err = 0;
    const size_t words = kChunk / 4;
    for (int pass = 0; pass < 3; pass++) {
        for (int c = 0; c < n; c++) {
            volatile uint32_t *p = chunks[c];
            for (size_t i = 0; i < words; i++) p[i] = psram_pattern(pass, &p[i]);
            for (size_t i = 0; i < words; i++) {
                if (p[i] != psram_pattern(pass, &p[i])) {
                    if (!errors) first_err = (uintptr_t)&p[i];
                    errors++;
                }
            }
            vTaskDelay(1);
        }
    }
    for (int c = 0; c < n; c++) heap_caps_free(chunks[c]);

    char msg[96];
    if (n == 0) {
        snprintf(msg, sizeof(msg), "Not enough free PSRAM\nA+B=back");
    } else if (errors == 0) {
        snprintf(msg, sizeof(msg), "PSRAM OK\n%u KB tested\n3 patterns, 0 errors\nA+B=back", (unsigned)(n * kChunk / 1024));
    } else {
        snprintf(msg, sizeof(msg), "PSRAM ERRORS: %u\nfirst at 0x%08X\n%u KB tested\nA+B=back",
                 (unsigned)errors, (unsigned)first_err, (unsigned)(n * kChunk / 1024));
    }
    show_and_wait(msg);
}

void k85_run_diagnostics(void) {
    static const char *items[] = {
        "Display test", "Buttons test", "Speaker test", "IMU test", "RTC test", "PSRAM test", "Back",
    };
    while (true) {
        int idx = k85_run_list_menu("DIAGNOSTICS", items, 7, nullptr);
        if (idx < 0 || idx == 6) return;
        if (idx == 0) test_display();
        else if (idx == 1) test_buttons();
        else if (idx == 2) test_speaker();
        else if (idx == 3) test_imu();
        else if (idx == 4) test_rtc();
        else if (idx == 5) test_psram();
    }
}

// ======================= Partitions & slots =======================
#define K85_MAX_PARTS 16

static int collect_partitions(const esp_partition_t **out, int max, bool apps_only) {
    int n = 0;
    esp_partition_iterator_t it = esp_partition_find(apps_only ? ESP_PARTITION_TYPE_APP : ESP_PARTITION_TYPE_ANY,
                                                     ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        if (n < max) out[n++] = p;
        it = esp_partition_next(it);
    }
    return n;
}

static void partition_table(void) {
    const esp_partition_t *parts[K85_MAX_PARTS];
    int n = collect_partitions(parts, K85_MAX_PARTS, false);
    char labels[K85_MAX_PARTS][40];
    const char *items[K85_MAX_PARTS + 1];
    for (int i = 0; i < n; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%-9.9s %6uK", parts[i]->label, (unsigned)(parts[i]->size / 1024));
        items[i] = labels[i];
    }
    items[n] = "Back";
    while (true) {
        int idx = k85_run_list_menu("PARTITIONS", items, n + 1, nullptr);
        if (idx < 0 || idx >= n) return;
        const esp_partition_t *p = parts[idx];
        char msg[160];
        snprintf(msg, sizeof(msg), "%s\n%s, subtype 0x%02X\n@ 0x%06X, %u KB\nencrypted: %s\nA+B=back",
                 p->label, p->type == ESP_PARTITION_TYPE_APP ? "app" : "data", (unsigned)p->subtype,
                 (unsigned)p->address, (unsigned)(p->size / 1024), p->encrypted ? "yes" : "no");
        show_and_wait(msg);
    }
}

static void slot_details(void) {
    const esp_partition_t *parts[K85_MAX_PARTS];
    int n = collect_partitions(parts, K85_MAX_PARTS, true);
    const esp_partition_t *running = esp_ota_get_running_partition();
    char labels[K85_MAX_PARTS][40];
    const char *items[K85_MAX_PARTS + 1];
    for (int i = 0; i < n; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%s%s", parts[i]->label, parts[i] == running ? " (running)" : "");
        items[i] = labels[i];
    }
    items[n] = "Back";
    while (true) {
        int idx = k85_run_list_menu("FIRMWARE SLOTS", items, n + 1, nullptr);
        if (idx < 0 || idx >= n) return;
        const esp_partition_t *p = parts[idx];
        esp_app_desc_t desc;
        char msg[200];
        if (esp_ota_get_partition_description(p, &desc) == ESP_OK) {
            snprintf(msg, sizeof(msg), "%s @0x%06X\nver: %.20s\napp: %.20s\nbuilt: %.12s %.8s\nA+B=back",
                     p->label, (unsigned)p->address, desc.version, desc.project_name, desc.date, desc.time);
        } else {
            snprintf(msg, sizeof(msg), "%s @0x%06X\nempty or invalid image\nA+B=back", p->label, (unsigned)p->address);
        }
        show_and_wait(msg);
    }
}

static void boot_other_slot(void) {
    const esp_partition_t *parts[K85_MAX_PARTS];
    int total = collect_partitions(parts, K85_MAX_PARTS, true);
    const esp_partition_t *running = esp_ota_get_running_partition();

    const esp_partition_t *cand[K85_MAX_PARTS];
    esp_app_desc_t descs[K85_MAX_PARTS > 4 ? 4 : K85_MAX_PARTS];
    char labels[4][48];
    const char *items[5];
    int n = 0;
    for (int i = 0; i < total && n < 4; i++) {
        const esp_partition_t *p = parts[i];
        if (p == running) continue;
        if (p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN || p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) continue;
        if (esp_ota_get_partition_description(p, &descs[n]) != ESP_OK) continue;
        cand[n] = p;
        snprintf(labels[n], sizeof(labels[n]), "%s  v%.16s", p->label, descs[n].version);
        items[n] = labels[n];
        n++;
    }
    if (n == 0) { show_and_wait("No other valid slot\nA+B=back"); return; }
    items[n] = "Back";

    int idx = k85_run_list_menu("BOOT SLOT NOW", items, n + 1, nullptr);
    if (idx < 0 || idx >= n) return;

    char msg[160];
    snprintf(msg, sizeof(msg), "Boot %s (%.16s)?\nPackage: %.24s\nB=confirm A+B=cancel",
             cand[idx]->label, descs[idx].project_name, g_config.alt_slot_name[0] ? g_config.alt_slot_name : "-");
    if (!confirm_msg(msg)) return;

    if (esp_ota_set_boot_partition(cand[idx]) != ESP_OK) { show_and_wait("Failed to set\nboot slot\nA+B=back"); return; }
    k85_show_message("Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

void k85_run_slots_menu(void) {
    static const char *items[] = { "Partition table", "Slot details", "Boot other slot", "Back" };
    while (true) {
        int idx = k85_run_list_menu("SLOTS & PARTITIONS", items, 4, nullptr);
        if (idx < 0 || idx == 3) return;
        if (idx == 0) partition_table();
        else if (idx == 1) slot_details();
        else if (idx == 2) boot_other_slot();
    }
}

// ======================= Security status =======================
void k85_run_security_status(void) {
    Pal p = pal();
    draw_frame("SECURITY STATUS", p);

    bool fenc = esp_flash_encryption_enabled();
    bool sboot = esp_secure_boot_enabled();
    bool jtag_pad_off = esp_efuse_read_field_bit(ESP_EFUSE_DIS_PAD_JTAG);
    bool jtag_usb_off = esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_JTAG);
    bool rom_dl_off = esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE);
    char tmp[8];
    bool bios_pw = k85_svc_get_str("bios_pw", tmp, sizeof(tmp));

    char line[48];
    snprintf(line, sizeof(line), "Flash encryption: %s", fenc ? "ON" : "OFF");        put_row(0, line, p);
    snprintf(line, sizeof(line), "Secure boot:      %s", sboot ? "ON" : "OFF");       put_row(1, line, p);
    snprintf(line, sizeof(line), "JTAG (pad):       %s", jtag_pad_off ? "disabled" : "enabled"); put_row(2, line, p);
    snprintf(line, sizeof(line), "JTAG (USB):       %s", jtag_usb_off ? "disabled" : "enabled"); put_row(3, line, p);
    snprintf(line, sizeof(line), "ROM download:     %s", rom_dl_off ? "disabled" : "enabled");   put_row(4, line, p);
    put_row(5, "OTA signature:    Ed25519 required", p);
    snprintf(line, sizeof(line), "BIOS password:    %s", bios_pw ? "ON" : "OFF");     put_row(6, line, p);
    put_row(8, "A+B=exit", p);
    wait_ab_exit();
}

#pragma GCC diagnostic pop
