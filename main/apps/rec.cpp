// K85REC: recovery from an SD card (boot loader menu -> Rec).
// Lists the .bin files on the card, checks that the image fits the free OTA slot and that it is an app
// image for the ESP32-S3, writes it into the free slot and goes back to the boot loader menu.
// No SD card / nothing flashed -> back to the boot loader menu as well.
#include "rec.h"
#include "rec_pins.h"
#include "common.h"
#include "theme.h"
#include "input.h"
#include "list_menu.h"
#include "log.h"
#include "boot_request.h"
#include "M5Unified.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

#define REC_MOUNT   "/sdcard"
#define REC_MAX     40

struct RecFile { char path[96]; uint32_t size; };
static RecFile s_files[REC_MAX];

static void rec_text_center(int y, const char *s, int size, uint32_t fg, uint32_t bg) {
    auto &d = M5.Display;
    int w = (int)strlen(s) * 6 * size;
    int x = (d.width() - w) / 2; if (x < 0) x = 0;
    d.setTextSize(size); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

static void rec_screen(const char *l1, const char *l2, const char *l3) {
    auto &d = M5.Display;
    uint32_t bg = k85_get_bg(), fg = k85_get_fg(), ac = k85_get_accent();
    d.fillScreen(bg); d.setTextWrap(false);
    int h = d.height();
    rec_text_center(h / 2 - 32, l1, 2, ac, bg);
    if (l2 && l2[0]) rec_text_center(h / 2 - 4, l2, 1, fg, bg);
    if (l3 && l3[0]) rec_text_center(h / 2 + 10, l3, 1, fg, bg);
}

static void rec_wait(uint32_t ms) {
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) < (int64_t)ms * 1000) {
        k85_input_update();
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void rec_bar(int pct, const char *txt) {
    auto &d = M5.Display;
    uint32_t bg = k85_get_bg(), fg = k85_get_fg(), ac = k85_get_accent();
    int w = d.width(), y = d.height() / 2 + 26, bw = w - 40;
    d.fillRect(20, y, bw, 10, bg);
    d.drawFastHLine(20, y, bw, fg); d.drawFastHLine(20, y + 9, bw, fg);
    d.fillRect(21, y + 1, (bw - 2) * pct / 100, 8, ac);
    d.fillRect(0, y + 14, w, 10, bg);
    rec_text_center(y + 14, txt, 1, fg, bg);
}

static void rec_exit(bool flashed) {
    k85_boot_request_post(flashed ? K85_BOOT_REQ_MENU_ALT : K85_BOOT_REQ_MENU);
    esp_restart();
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

static bool sd_mount(sdmmc_card_t **card) {
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
    if (esp_vfs_fat_sdspi_mount(REC_MOUNT, &host, &sl, &mc, card) != ESP_OK) {
        spi_bus_free((spi_host_device_t)host.slot);
        return false;
    }
    return true;
}

static void sd_unmount(sdmmc_card_t *card) {
    esp_vfs_fat_sdcard_unmount(REC_MOUNT, card);
    spi_bus_free(SPI2_HOST);
}

static bool is_bin(const char *n) {
    size_t L = strlen(n);
    if (L < 5) return false;
    const char *e = n + L - 4;
    return e[0] == '.' && (e[1] | 32) == 'b' && (e[2] | 32) == 'i' && (e[3] | 32) == 'n';
}

static int scan_dir(const char *dir, int n) {
    DIR *d = opendir(dir);
    if (!d) return n;
    struct dirent *e;
    while (n < REC_MAX && (e = readdir(d)) != nullptr) {
        if (strlen(e->d_name) > 56 || !is_bin(e->d_name)) continue;
        char path[96];
        snprintf(path, sizeof(path), "%.24s/%.56s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(s_files[n].path, sizeof(s_files[n].path), "%s", path);
        s_files[n].size = (uint32_t)st.st_size;
        n++;
    }
    closedir(d);
    return n;
}

static void fmt_size(char *o, size_t n, uint32_t b) {
    if (b >= 1024u * 1024u) snprintf(o, n, "%u.%uM", (unsigned)(b >> 20), (unsigned)(((b & 0xFFFFF) * 10) >> 20));
    else snprintf(o, n, "%uK", (unsigned)((b + 1023) / 1024));
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

// returns true when the image was written
static bool rec_flash(const RecFile &f) {
    const esp_partition_t *dst = esp_ota_get_next_update_partition(nullptr);
    char l2[64], l3[64];
    if (!dst) { rec_screen("No free slot", "only one app slot", "back to SD menu"); rec_wait(3500); return false; }
    if (f.size > dst->size) {
        snprintf(l2, sizeof(l2), "%u KB > slot %u KB", (unsigned)(f.size / 1024), (unsigned)(dst->size / 1024));
        rec_screen("Too big", l2, "back to SD menu");
        rec_wait(4500);
        return false;
    }
    FILE *fp = fopen(f.path, "rb");
    if (!fp) { rec_screen("Cannot open file", base_name(f.path), "back to SD menu"); rec_wait(3000); return false; }
    uint8_t hdr[64];
    size_t got = fread(hdr, 1, sizeof(hdr), fp);
    uint32_t magic = 0;
    uint16_t chip = 0;
    if (got == sizeof(hdr)) { memcpy(&magic, hdr + 32, sizeof(magic)); memcpy(&chip, hdr + 12, sizeof(chip)); }
    if (got != sizeof(hdr) || hdr[0] != 0xE9 || magic != 0xABCD5432u) {
        fclose(fp);
        rec_screen("Not an app image", "need an application .bin", "(not merged / bootloader)");
        rec_wait(4000);
        return false;
    }
    if (chip != 9) {                       /* ESP_CHIP_ID_ESP32S3 */
        fclose(fp);
        snprintf(l2, sizeof(l2), "image is for chip id %u", (unsigned)chip);
        rec_screen("Wrong chip", l2, "back to SD menu");
        rec_wait(4000);
        return false;
    }

    char t1[48];
    snprintf(t1, sizeof(t1), "Flash to %.8s (%uK)", dst->label, (unsigned)(f.size / 1024));
    const char *items[2] = { t1, "Cancel" };
    const char *keys[2] = { nullptr, nullptr };
    int c = k85_run_list_menu(base_name(f.path), items, 2, keys);
    if (c != 0) { fclose(fp); return false; }

    rec_screen("Rec", "erasing slot...", dst->label);
    esp_ota_handle_t h = 0;
    if (esp_ota_begin(dst, f.size, &h) != ESP_OK) {
        fclose(fp);
        rec_screen("Cannot start", "ota begin failed", "back to SD menu");
        rec_wait(3500);
        return false;
    }
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) { fclose(fp); esp_ota_abort(h); rec_screen("Out of memory", "", "back to SD menu"); rec_wait(3000); return false; }
    fseek(fp, 0, SEEK_SET);
    rec_screen("Rec", "writing...", dst->label);
    uint32_t done = 0;
    bool ok = true;
    int last_pct = -1;
    while (done < f.size) {
        size_t want = f.size - done < 4096 ? f.size - done : 4096;
        size_t n = fread(buf, 1, want, fp);
        if (n == 0) { ok = false; break; }
        if (esp_ota_write(h, buf, n) != ESP_OK) { ok = false; break; }
        done += (uint32_t)n;
        int pct = (int)(((uint64_t)done * 100) / f.size);
        if (pct != last_pct) {
            last_pct = pct;
            snprintf(l3, sizeof(l3), "%u / %u KB", (unsigned)(done / 1024), (unsigned)(f.size / 1024));
            rec_bar(pct, l3);
        }
    }
    free(buf);
    fclose(fp);
    if (!ok) {
        esp_ota_abort(h);
        rec_screen("Write failed", "file read / flash error", "back to SD menu");
        rec_wait(4000);
        return false;
    }
    if (esp_ota_end(h) != ESP_OK) {
        rec_screen("Image invalid", "check failed after write", "back to SD menu");
        rec_wait(4000);
        return false;
    }
    k85_log("Rec: %.40s -> %.8s", base_name(f.path), dst->label);
    snprintf(l2, sizeof(l2), "written to %.8s", dst->label);
    rec_screen("Done", l2, "boot menu: Alt Firmware");
    rec_wait(3000);
    return true;
}

void k85_run_rec(void) {
    M5.Display.setTextSize(1);
    rec_screen("Rec", "looking for SD card...", "");
    sdmmc_card_t *card = nullptr;
    if (!sd_mount(&card)) {
        rec_screen("No SD card", "back to boot menu", "");
        rec_wait(2500);
        rec_exit(false);
    }
    bool flashed = false;
    while (true) {
        int n = scan_dir(REC_MOUNT, 0);
        n = scan_dir(REC_MOUNT "/firmware", n);
        if (n == 0) { rec_screen("No .bin files", "put firmware on the card", "(root or /firmware)"); rec_wait(3500); break; }
        static char labels[REC_MAX + 1][40];
        const char *items[REC_MAX + 1];
        const char *keys[REC_MAX + 1];
        for (int i = 0; i < n; i++) {
            char sz[12];
            fmt_size(sz, sizeof(sz), s_files[i].size);
            snprintf(labels[i], sizeof(labels[i]), "%.24s %s", base_name(s_files[i].path), sz);
            items[i] = labels[i]; keys[i] = nullptr;
        }
        snprintf(labels[n], sizeof(labels[n]), "<- boot menu");
        items[n] = labels[n]; keys[n] = nullptr;
        int sel = k85_run_list_menu("Rec: SD card", items, n + 1, keys);
        if (sel < 0 || sel >= n) break;
        if (rec_flash(s_files[sel])) { flashed = true; break; }
    }
    sd_unmount(card);
    rec_exit(flashed);
}