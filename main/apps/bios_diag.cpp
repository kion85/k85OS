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
#include "battery.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "rec_pins.h"
#include <cstdlib>
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

// ======================= Storage / boot / power info =======================
static const char *dg_flash_vendor(uint8_t m) {
    switch (m) {
        case 0xEF: return "Winbond";
        case 0xC8: return "GigaDevice";
        case 0x20: return "XMC/Micron";
        case 0xC2: return "Macronix";
        case 0x85: return "Puya";
        case 0x0B: return "XTX";
        case 0x1C: return "EON";
        case 0xA1: return "Fudan";
        default:   return "unknown";
    }
}

// same SD wiring and bus (SPI2) as the Rec app and the backup feature
static bool dg_sd_mount(sdmmc_card_t **card) {
    esp_vfs_fat_mount_config_t mc = {};
    mc.format_if_mount_failed = false;
    mc.max_files = 4;
    mc.allocation_unit_size = 16 * 1024;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;
    spi_bus_config_t bus = {};
    bus.mosi_io_num = K85_SD_MOSI;
    bus.miso_io_num = K85_SD_MISO;
    bus.sclk_io_num = K85_SD_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t e = spi_bus_initialize((spi_host_device_t)host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
    sdspi_device_config_t sl = SDSPI_DEVICE_CONFIG_DEFAULT();
    sl.gpio_cs = (gpio_num_t)K85_SD_CS;
    sl.host_id = (spi_host_device_t)host.slot;
    if (esp_vfs_fat_sdspi_mount("/sdcard", &host, &sl, &mc, card) != ESP_OK) {
        spi_bus_free((spi_host_device_t)host.slot);
        return false;
    }
    return true;
}

static void dg_sd_unmount(sdmmc_card_t *card) {
    esp_vfs_fat_sdcard_unmount("/sdcard", card);
    spi_bus_free(SPI2_HOST);
}

static void dg_sd_test(const Pal &p) {
    char l[48];
    put_row(5, "SD: mounting ...", p);
    put_row(6, "", p);
    put_row(7, "", p);
    sdmmc_card_t *card = nullptr;
    if (!dg_sd_mount(&card)) { put_row(5, "SD: no card / mount failed", p); return; }
    uint64_t bytes = (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
    snprintf(l, sizeof(l), "SD: %.5s  %u MB", card->cid.name, (unsigned)(bytes >> 20));
    put_row(5, l, p);

    const size_t kBlk = 4096, kCount = 64;                    // 256 KB test file
    uint8_t *wb = (uint8_t *)malloc(kBlk);
    uint8_t *rb = (uint8_t *)malloc(kBlk);
    if (!wb || !rb) {
        free(wb); free(rb);
        dg_sd_unmount(card);
        put_row(6, "Not enough memory", p);
        return;
    }
    for (size_t i = 0; i < kBlk; i++) wb[i] = (uint8_t)(i * 31 + 7);
    const char *path = "/sdcard/k85_test.tmp";

    int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(path, "wb");
    bool ok = (f != nullptr);
    for (size_t i = 0; ok && i < kCount; i++) ok = (fwrite(wb, 1, kBlk, f) == kBlk);
    if (f && fclose(f) != 0) ok = false;
    int64_t t1 = esp_timer_get_time();
    if (!ok) {
        remove(path);
        free(wb); free(rb);
        dg_sd_unmount(card);
        put_row(6, "Write FAILED (card locked/full?)", p);
        return;
    }
    int64_t dw = t1 - t0;
    snprintf(l, sizeof(l), "Write: %u KB/s", (unsigned)((uint64_t)kBlk * kCount * 1000000ULL / (uint64_t)(dw > 0 ? dw : 1) / 1024));
    put_row(6, l, p);

    int64_t t2 = esp_timer_get_time();
    bool same = true;
    f = fopen(path, "rb");
    if (!f) same = false;
    for (size_t i = 0; f && i < kCount; i++) {
        if (fread(rb, 1, kBlk, f) != kBlk || memcmp(rb, wb, kBlk) != 0) { same = false; break; }
    }
    if (f) fclose(f);
    int64_t t3 = esp_timer_get_time();
    remove(path);
    free(wb); free(rb);
    dg_sd_unmount(card);
    int64_t dr = t3 - t2;
    snprintf(l, sizeof(l), "Read: %u KB/s  %s", (unsigned)((uint64_t)kBlk * kCount * 1000000ULL / (uint64_t)(dr > 0 ? dr : 1) / 1024),
             same ? "data OK" : "DATA MISMATCH");
    put_row(7, l, p);
}

static void dg_storage(void) {
    Pal p = pal();
    draw_frame("STORAGE TEST", p);
    uint32_t id = 0, fsz = 0;
    esp_flash_read_id(nullptr, &id);
    esp_flash_get_size(nullptr, &fsz);
    size_t tot = 0, used = 0;
    bool fs_ok = k85_fs_info(&tot, &used);
    uint8_t mf = (uint8_t)(id >> 16);
    char l[48];
    snprintf(l, sizeof(l), "Flash: %u MB   ID %06X", (unsigned)(fsz >> 20), (unsigned)id);
    put_row(0, l, p);
    snprintf(l, sizeof(l), "Vendor: %s", dg_flash_vendor(mf));
    put_row(1, l, p);
    put_row(2, mf == 0x20 ? "XMC flash: keep XMC support ON" : "XMC support not needed", p);
    if (fs_ok) snprintf(l, sizeof(l), "LittleFS: %u / %u KB used", (unsigned)(used / 1024), (unsigned)(tot / 1024));
    else snprintf(l, sizeof(l), "LittleFS: n/a");
    put_row(3, l, p);
    put_row(8, "B=SD speed test   A+B=exit", p);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_b_pressed()) dg_sd_test(p);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static const char *dg_reset_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external pin";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "PANIC (crash)";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (low power)";
        default:                return "other";
    }
}

// crash-loop guard log, written by the boot loader hook (spare sector 0x14000): crash resets since the last normal boot
static int dg_crash_streak(void) {
    uint32_t w[32];
    int streak = 0;
    for (uint32_t base = 0; base < 1024; base += 32) {
        if (esp_flash_read(nullptr, w, 0x14000 + base * 4, sizeof(w)) != ESP_OK) return -1;
        for (int i = 0; i < 32; i++) {
            if (w[i] == 0xFFFFFFFFu) return streak;
            if (w[i] == 0x4B4C0002u) streak = 0;
            else if (w[i] == 0x4B4C0001u) streak++;
        }
    }
    return streak;
}

static void dg_boot_info(void) {
    Pal p = pal();
    draw_frame("BOOT INFO", p);
    char l[48];
    snprintf(l, sizeof(l), "Reset: %s", dg_reset_name(esp_reset_reason()));
    put_row(0, l, p);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    snprintf(l, sizeof(l), "Chip: S3 rev v%d.%d, %d cores", (int)(ci.revision / 100), (int)(ci.revision % 100), (int)ci.cores);
    put_row(1, l, p);
    snprintf(l, sizeof(l), "IDF: %.28s", esp_get_idf_version());
    put_row(2, l, p);
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) {
        esp_app_desc_t d;
        if (esp_ota_get_partition_description(run, &d) == ESP_OK) snprintf(l, sizeof(l), "App: %.14s v%.14s", d.project_name, d.version);
        else snprintf(l, sizeof(l), "App: n/a");
        put_row(3, l, p);
        const char *ss = "n/a";
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
            switch (st) {
                case ESP_OTA_IMG_NEW:            ss = "new"; break;
                case ESP_OTA_IMG_PENDING_VERIFY: ss = "pending verify"; break;
                case ESP_OTA_IMG_VALID:          ss = "valid"; break;
                case ESP_OTA_IMG_INVALID:        ss = "invalid"; break;
                case ESP_OTA_IMG_ABORTED:        ss = "aborted"; break;
                default:                         ss = "undefined"; break;
            }
        }
        snprintf(l, sizeof(l), "Slot: %s @0x%06X  %s", run->label, (unsigned)run->address, ss);
        put_row(4, l, p);
    }
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(l, sizeof(l), "MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    put_row(5, l, p);
    snprintf(l, sizeof(l), "Min free heap: %u KB", (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    put_row(6, l, p);
    { int cs = dg_crash_streak(); char cl[48]; if (cs >= 0) snprintf(cl, sizeof(cl), "Crash streak: %d (guard at 3)", cs); else snprintf(cl, sizeof(cl), "Crash streak: n/a"); put_row(7, cl, p); }
    put_row(8, "A+B=exit", p);
    wait_ab_exit();
}

static void dg_power(void) {
    Pal p = pal();
    draw_frame("POWER INFO", p);
    put_row(8, "A+B=exit", p);
    int64_t last = 0;
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        int64_t now = esp_timer_get_time();
        if (last == 0 || now - last >= 1000000) {
            last = now;
            char l[48];
            int pct = k85_get_battery();
            int32_t mv = M5.Power.getBatteryVoltage();
            int32_t vbus = M5.Power.getVBUSVoltage();
            int32_t cur = M5.Power.getBatteryCurrent();
            int chg = (int)M5.Power.isCharging();
            if (pct >= 0) snprintf(l, sizeof(l), "Battery: %d%%   %.2f V", pct, mv / 1000.0f);
            else snprintf(l, sizeof(l), "Battery: n/a   %.2f V", mv / 1000.0f);
            put_row(0, l, p);
            snprintf(l, sizeof(l), "State: %s", chg == 1 ? "charging" : (chg == 0 ? "discharging" : "unknown"));
            put_row(1, l, p);
            if (vbus > 0) snprintf(l, sizeof(l), "USB (VBUS): %.2f V", vbus / 1000.0f);
            else snprintf(l, sizeof(l), "USB (VBUS): not connected");
            put_row(2, l, p);
            snprintf(l, sizeof(l), "Battery current: %d mA", (int)cur);
            put_row(3, l, p);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// Diagnostics -> Crash test: a deliberate abort() to try the boot loader crash-loop guard (3 crashes in a row)
static void dg_crash_test(void) {
    if (!confirm_msg("Crash the firmware now?\nTests the crash-loop guard\nof the boot loader\nB=confirm A+B=cancel")) return;
    k85_show_message("Crashing ...");
    vTaskDelay(pdMS_TO_TICKS(600));
    abort();
}

void k85_run_diagnostics(void) {
    static const char *items[] = {
        "Display test", "Buttons test", "Speaker test", "IMU test", "RTC test", "PSRAM test", "Storage test", "Boot info", "Power info", "Crash test", "Back",
    };
    while (true) {
        int idx = k85_run_list_menu("DIAGNOSTICS", items, 11, nullptr);
        if (idx < 0 || idx == 10) return;
        if (idx == 0) test_display();
        else if (idx == 1) test_buttons();
        else if (idx == 2) test_speaker();
        else if (idx == 3) test_imu();
        else if (idx == 4) test_rtc();
        else if (idx == 5) test_psram();
        else if (idx == 6) dg_storage();
        else if (idx == 7) dg_boot_info();
        else if (idx == 8) dg_power();
        else if (idx == 9) dg_crash_test();
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
