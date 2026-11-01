// K85LOOK1: user wallpapers and the status bar banner.
// Images are read from /littlefs/wall. Each one is decoded ONCE, scaled to cover the screen and dimmed;
// the ready frame is kept in PSRAM and in a cache file, so drawing it later is a single fast copy.
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "look.h"
#include "theme.h"
#include "common.h"
#include "list_menu.h"
#include "input.h"
#include "log.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOOK_FILE     "/littlefs/k85_look.txt"
#define WALL_DIR      "/littlefs/wall"
#define CACHE_DIR     "/littlefs/wallcache"
#define LOOK_NAME_MAX 48
#define LOOK_IMG_MAX  24
#define BANNER_H      12
#define CACHE_MAGIC   0x4B38574CU

enum { LOOK_OK = 0, LOOK_ERR_READ, LOOK_ERR_FORMAT, LOOK_ERR_PROGRESSIVE, LOOK_ERR_SIZE, LOOK_ERR_MEM, LOOK_ERR_DECODE };

struct LookCfg {
    char wall[3][LOOK_NAME_MAX];
    uint8_t dim[3];
    char banner[LOOK_NAME_MAX];
    uint8_t bdim;
};
static LookCfg s_cfg;
static bool s_loaded = false;
static uint32_t s_serial = 1;      // bumped whenever the settings change

static const uint8_t DIM_STEPS[] = { 0, 25, 40, 55, 70, 85 };
#define DIM_STEP_COUNT (int)(sizeof(DIM_STEPS) / sizeof(DIM_STEPS[0]))

struct Surf { M5Canvas *cv; int w, h; uint32_t serial, key; bool failed; int err; };   // K85LOOK2
static Surf s_wall[3];
static Surf s_ban;
static Surf s_img;   // background of a lock scene

static const char *tag_for(int slot) { return slot == 0 ? "lock" : (slot == 1 ? "main" : "sys"); }
static int clamp_dim(int d) { return d < 0 ? 0 : (d > 90 ? 90 : d); }

// ---------------- settings file ----------------
static void cfg_load(void) {
    if (s_loaded) return;
    s_loaded = true;
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.dim[0] = s_cfg.dim[1] = s_cfg.dim[2] = 40;
    s_cfg.bdim = 40;
    FILE *f = fopen(LOOK_FILE, "r");
    if (!f) return;
    char line[112];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line;
        const char *v = eq + 1;
        for (int i = 0; i < 3; i++) {
            char kn[8], kd[8];
            snprintf(kn, sizeof(kn), "wall%d", i);
            snprintf(kd, sizeof(kd), "dim%d", i);
            if (!strcmp(k, kn)) snprintf(s_cfg.wall[i], LOOK_NAME_MAX, "%.47s", v);
            else if (!strcmp(k, kd)) s_cfg.dim[i] = (uint8_t)clamp_dim(atoi(v));
        }
        if (!strcmp(k, "banner")) snprintf(s_cfg.banner, LOOK_NAME_MAX, "%.47s", v);
        else if (!strcmp(k, "bdim")) s_cfg.bdim = (uint8_t)clamp_dim(atoi(v));
    }
    fclose(f);
}

static void cfg_save(void) {
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%s.tmp", LOOK_FILE);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < 3; i++) {
        fprintf(f, "wall%d=%s\n", i, s_cfg.wall[i]);
        fprintf(f, "dim%d=%d\n", i, (int)s_cfg.dim[i]);
    }
    fprintf(f, "banner=%s\n", s_cfg.banner);
    fprintf(f, "bdim=%d\n", (int)s_cfg.bdim);
    bool ok = (ferror(f) == 0);
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return; }
    remove(LOOK_FILE);
    rename(tmp, LOOK_FILE);
}

// ---------------- image header ----------------
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t fnv(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; } return h; }

// type: 1 = JPEG, 2 = PNG, 3 = BMP, 4 = QOI. Reads only the header, never the whole file.
static bool image_info(const char *path, int *type, int *iw, int *ih, bool *progressive) {
    *progressive = false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[32];
    size_t n = fread(h, 1, sizeof(h), f);
    bool ok = false;
    if (n >= 26 && h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') {
        *type = 2; *iw = (int)be32(h + 16); *ih = (int)be32(h + 20); ok = true;
    } else if (n >= 26 && h[0] == 'B' && h[1] == 'M') {
        int32_t bw = (int32_t)le32(h + 18), bh = (int32_t)le32(h + 22);
        *type = 3; *iw = bw; *ih = bh < 0 ? -bh : bh; ok = true;
    } else if (n >= 14 && memcmp(h, "qoif", 4) == 0) {
        *type = 4; *iw = (int)be32(h + 4); *ih = (int)be32(h + 8); ok = true;
    } else if (n >= 4 && h[0] == 0xFF && h[1] == 0xD8) {
        *type = 1;
        long pos = 2;
        while (pos < 4L * 1024 * 1024) {
            if (fseek(f, pos, SEEK_SET) != 0) break;
            uint8_t m[4];
            if (fread(m, 1, 4, f) != 4) break;
            if (m[0] != 0xFF) { pos++; continue; }
            uint8_t mk = m[1];
            if (mk == 0xFF) { pos++; continue; }
            if (mk == 0xD8 || mk == 0x01 || (mk >= 0xD0 && mk <= 0xD7)) { pos += 2; continue; }
            int len = (m[2] << 8) | m[3];
            if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
                uint8_t s[5];
                if (fread(s, 1, 5, f) != 5) break;
                *ih = (s[1] << 8) | s[2];
                *iw = (s[3] << 8) | s[4];
                *progressive = (mk != 0xC0 && mk != 0xC1);
                ok = true;
                break;
            }
            if (len < 2) break;
            pos += 2 + len;
        }
    }
    fclose(f);
    return ok && *iw > 0 && *ih > 0;
}

static bool draw_file(M5Canvas *cv, int type, const char *path, int x, int y, float sc) {
    switch (type) {
        case 1: return cv->drawJpgFile(path, x, y, 0, 0, 0, 0, sc, sc);
        case 2: return cv->drawPngFile(path, x, y, 0, 0, 0, 0, sc, sc);
        case 3: return cv->drawBmpFile(path, x, y, 0, 0, 0, 0, sc, sc);
        case 4: return cv->drawQoiFile(path, x, y, 0, 0, 0, 0, sc, sc);
    }
    return false;
}

// ---------------- ready-frame cache ----------------
struct CacheHdr { uint32_t magic, name_hash, src_size, data_len; uint16_t w, h; uint8_t dim, ver; uint16_t pad; };

static void cache_path(char *out, size_t cap, const char *tag, int w, int h) {
    snprintf(out, cap, "%s/%s_%dx%d.bin", CACHE_DIR, tag, w, h);
}

static bool cache_load(M5Canvas *cv, const char *cpath, uint32_t nh, bool have_src, uint32_t src_size, int w, int h, int dim) {
    FILE *f = fopen(cpath, "rb");
    if (!f) return false;
    CacheHdr hd;
    bool ok = fread(&hd, 1, sizeof(hd), f) == sizeof(hd) && hd.magic == CACHE_MAGIC && hd.ver == 1 &&
              hd.name_hash == nh && hd.w == w && hd.h == h && hd.dim == dim &&
              (!have_src || hd.src_size == src_size) && hd.data_len == (uint32_t)cv->bufferLength();
    if (ok) ok = fread(cv->getBuffer(), 1, hd.data_len, f) == hd.data_len;
    fclose(f);
    return ok;
}

static void cache_save(M5Canvas *cv, const char *cpath, uint32_t nh, uint32_t src_size, int w, int h, int dim) {
    mkdir(CACHE_DIR, 0755);
    char tmp[112];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cpath);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    CacheHdr hd;
    memset(&hd, 0, sizeof(hd));
    hd.magic = CACHE_MAGIC; hd.ver = 1; hd.name_hash = nh; hd.src_size = src_size;
    hd.w = (uint16_t)w; hd.h = (uint16_t)h; hd.dim = (uint8_t)dim;
    hd.data_len = (uint32_t)cv->bufferLength();
    bool ok = fwrite(&hd, 1, sizeof(hd), f) == sizeof(hd) && fwrite(cv->getBuffer(), 1, hd.data_len, f) == hd.data_len;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return; }
    remove(cpath);
    rename(tmp, cpath);
}

static void purge_cache(const char *tag) {
    DIR *d = opendir(CACHE_DIR);
    if (!d) return;
    char prefix[12];
    snprintf(prefix, sizeof(prefix), "%s_", tag);
    size_t pl = strlen(prefix);
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (strncmp(e->d_name, prefix, pl) != 0) continue;
        char p[96];
        snprintf(p, sizeof(p), "%s/%.60s", CACHE_DIR, e->d_name);
        remove(p);
    }
    closedir(d);
}

// ---------------- building a frame ----------------
static void surf_free(Surf &s) {
    if (s.cv) { s.cv->deleteSprite(); delete s.cv; s.cv = nullptr; }
}

static int bail(Surf &s, M5Canvas *cv, int e) {
    if (cv) { cv->deleteSprite(); delete cv; }
    s.failed = true;
    s.err = e;
    return e;
}

static int surf_build(Surf &s, const char *name, int dim, int w, int h, const char *tag) {
    uint32_t k85_key = fnv(name) ^ ((uint32_t)dim * 2654435761u);
    if (s.serial == s_serial && s.w == w && s.h == h && s.key == k85_key) return s.failed ? s.err : LOOK_OK;
    surf_free(s);
    s.w = w; s.h = h; s.serial = s_serial; s.key = k85_key; s.failed = false; s.err = LOOK_OK;

    char path[96];
    snprintf(path, sizeof(path), "%s/%.47s", WALL_DIR, name);
    struct stat sb;
    bool have_src = (stat(path, &sb) == 0);
    uint32_t src_size = have_src ? (uint32_t)sb.st_size : 0;
    uint32_t nh = fnv(name);

    M5Canvas *cv = new M5Canvas(&M5.Display);
    if (!cv) return bail(s, nullptr, LOOK_ERR_MEM);
    cv->setColorDepth(16);
    cv->setPsram(true);
    if (!cv->createSprite(w, h)) return bail(s, cv, LOOK_ERR_MEM);

    char cpath[96];
    cache_path(cpath, sizeof(cpath), tag, w, h);
    if (cache_load(cv, cpath, nh, have_src, src_size, w, h, dim)) { s.cv = cv; return LOOK_OK; }
    if (!have_src) return bail(s, cv, LOOK_ERR_READ);

    int type = 0, iw = 0, ih = 0;
    bool prog = false;
    if (!image_info(path, &type, &iw, &ih, &prog)) return bail(s, cv, LOOK_ERR_FORMAT);
    if (prog) return bail(s, cv, LOOK_ERR_PROGRESSIVE);
    if (iw > 16000 || ih > 16000) return bail(s, cv, LOOK_ERR_SIZE);

    cv->fillScreen(0x000000);
    float sx = (float)w / (float)iw, sy = (float)h / (float)ih;
    float sc = sx > sy ? sx : sy;                                   // cover: fill the screen, crop the rest
    int x0 = (int)(((float)w - (float)iw * sc) * 0.5f);
    int y0 = (int)(((float)h - (float)ih * sc) * 0.5f);
    if (!draw_file(cv, type, path, x0, y0, sc)) return bail(s, cv, LOOK_ERR_DECODE);
    if (dim > 0) cv->fillRectAlpha(0, 0, w, h, (uint8_t)(dim * 255 / 100), 0x000000);
    cache_save(cv, cpath, nh, src_size, w, h, dim);
    s.cv = cv;
    return LOOK_OK;
}

// ---------------- public drawing API ----------------
bool k85_wall_active(K85WallSlot slot) {
    cfg_load();
    if (!s_cfg.wall[slot][0]) return false;
    const Surf &s = s_wall[slot];
    return !(s.serial == s_serial && s.failed);
}

void k85_wall_draw(K85WallSlot slot, uint32_t fallback_bg) {
    cfg_load();
    if (s_cfg.wall[slot][0]) {
        int e = surf_build(s_wall[slot], s_cfg.wall[slot], s_cfg.dim[slot], M5.Display.width(), M5.Display.height(), tag_for(slot));
        if (e == LOOK_OK && s_wall[slot].cv) { s_wall[slot].cv->pushSprite(&M5.Display, 0, 0); return; }
    }
    M5.Display.fillScreen(fallback_bg);
}

void k85_wall_text(uint32_t fg, uint32_t bg) {
    if (bg == k85_get_bg() && k85_wall_active(K85_WALL_SYS)) M5.Display.setTextColor(fg);   // no solid box under the text
    else M5.Display.setTextColor(fg, bg);
}

bool k85_banner_active(void) {
    cfg_load();
    if (!s_cfg.banner[0]) return false;
    return !(s_ban.serial == s_serial && s_ban.failed);
}

bool k85_banner_draw(int x0, int wpart) {
    cfg_load();
    if (!s_cfg.banner[0]) return false;
    int w = M5.Display.width();
    if (surf_build(s_ban, s_cfg.banner, s_cfg.bdim, w, BANNER_H, "ban") != LOOK_OK || !s_ban.cv) return false;
    M5.Display.setClipRect(x0, 0, wpart, BANNER_H);
    s_ban.cv->pushSprite(&M5.Display, 0, 0);
    M5.Display.clearClipRect();
    return true;
}

// ---------------- settings menus ----------------
static void wait_ab(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static const char *err_text(int e) {
    switch (e) {
        case LOOK_ERR_READ:        return "Cannot read the file";
        case LOOK_ERR_FORMAT:      return "Not a JPG, PNG, BMP\nor QOI image";
        case LOOK_ERR_PROGRESSIVE: return "Progressive JPEG is\nnot supported.\nSave it as baseline";
        case LOOK_ERR_SIZE:        return "Image is too large\n(max 16000 px)";
        case LOOK_ERR_MEM:         return "Not enough memory";
        default:                   return "Cannot decode\nthe image";
    }
}

// which: 0..2 = wallpaper slot, 3 = status bar banner
static char *which_name(int which) { return which == 3 ? s_cfg.banner : s_cfg.wall[which]; }
static uint8_t *which_dim(int which) { return which == 3 ? &s_cfg.bdim : &s_cfg.dim[which]; }
static const char *which_title(int which) { static const char *tt[] = { "WALL: LOCK", "WALL: MAIN", "WALL: SYSTEM", "BANNER" }; return tt[(which < 0 || which > 3) ? 3 : which]; }

static int which_build(int which) {
    int dw = M5.Display.width(), dh = M5.Display.height();
    if (which == 3) return surf_build(s_ban, s_cfg.banner, s_cfg.bdim, dw, BANNER_H, "ban");
    return surf_build(s_wall[which], s_cfg.wall[which], s_cfg.dim[which], dw, dh, tag_for(which));
}

static void apply_and_report(int which) {
    s_serial++;
    k85_show_message("Preparing...\n(can take a few\nseconds)");
    int e = which_build(which);
    if (e == LOOK_OK) {
        k85_show_message("Done\nA+B=back");
    } else {
        char m[96];
        snprintf(m, sizeof(m), "%s\nA+B=back", err_text(e));
        k85_show_message(m);
        which_name(which)[0] = 0;       // do not keep a file that cannot be used
        cfg_save();
        s_serial++;
    }
    wait_ab();
}

static int list_images(char names[][LOOK_NAME_MAX], int max) {
    DIR *d = opendir(WALL_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && n < max) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (nm[0] == '.' || l < 5 || l >= LOOK_NAME_MAX) continue;
        const char *dot = strrchr(nm, '.');
        if (!dot) continue;
        char ext[8];
        size_t i = 0;
        for (; dot[i] && i < 7; i++) ext[i] = (char)((dot[i] >= 'A' && dot[i] <= 'Z') ? dot[i] + 32 : dot[i]);
        ext[i] = 0;
        if (strcmp(ext, ".jpg") && strcmp(ext, ".jpeg") && strcmp(ext, ".png") && strcmp(ext, ".bmp") && strcmp(ext, ".qoi")) continue;
        snprintf(names[n++], LOOK_NAME_MAX, "%s", nm);
    }
    closedir(d);
    return n;
}

static void choose_image(int which) {
    static char names[LOOK_IMG_MAX][LOOK_NAME_MAX];
    static char lbl[LOOK_IMG_MAX][24];
    int n = list_images(names, LOOK_IMG_MAX);
    if (n == 0) {
        k85_show_message("No images found.\nUpload JPG/PNG/BMP\nto /wall (FTP or\nweb files)\nA+B=back");
        wait_ab();
        return;
    }
    const char *items[LOOK_IMG_MAX + 1];
    for (int i = 0; i < n; i++) { snprintf(lbl[i], sizeof(lbl[i]), "%.20s", names[i]); items[i] = lbl[i]; }
    items[n] = "Back";
    int idx = k85_run_list_menu("CHOOSE IMAGE", items, n + 1, nullptr);
    if (idx < 0 || idx >= n) return;
    snprintf(which_name(which), LOOK_NAME_MAX, "%s", names[idx]);
    cfg_save();
    apply_and_report(which);
}

static void image_menu(int which) {
    while (true) {
        char l_img[40], l_dim[24];
        snprintf(l_img, sizeof(l_img), "Image: %.22s", which_name(which)[0] ? which_name(which) : "none");
        snprintf(l_dim, sizeof(l_dim), "Dim: %d%%", (int)*which_dim(which));
        const char *items[] = { l_img, l_dim, "Remove", "Back" };
        int idx = k85_run_list_menu(which_title(which), items, 4, nullptr);
        if (idx < 0 || idx == 3) return;
        if (idx == 0) {
            choose_image(which);
        } else if (idx == 1) {
            int cur = 0;
            for (int i = 0; i < DIM_STEP_COUNT; i++) if (DIM_STEPS[i] <= *which_dim(which)) cur = i;
            *which_dim(which) = DIM_STEPS[(cur + 1) % DIM_STEP_COUNT];
            cfg_save();
            if (which_name(which)[0]) apply_and_report(which);
        } else if (idx == 2) {
            which_name(which)[0] = 0;
            if (which == 3) { surf_free(s_ban); purge_cache("ban"); }
            else { surf_free(s_wall[which]); purge_cache(tag_for(which)); }
            s_serial++;
            cfg_save();
        }
    }
}

void k85_wall_text_main(uint32_t fg, uint32_t bg) {
    if (bg == k85_get_bg() && k85_wall_active(K85_WALL_MAIN)) M5.Display.setTextColor(fg);
    else M5.Display.setTextColor(fg, bg);
}

bool k85_wall_push(K85WallSlot slot, lgfx::LovyanGFX *dst) {
    cfg_load();
    if (!s_cfg.wall[slot][0]) return false;
    int e = surf_build(s_wall[slot], s_cfg.wall[slot], s_cfg.dim[slot], M5.Display.width(), M5.Display.height(), tag_for(slot));
    if (e != LOOK_OK || !s_wall[slot].cv) return false;
    s_wall[slot].cv->pushSprite(dst, 0, 0);
    return true;
}

bool k85_image_push(const char *name, int dim, lgfx::LovyanGFX *dst) {
    if (!name || !name[0]) return false;
    cfg_load();
    int e = surf_build(s_img, name, clamp_dim(dim), M5.Display.width(), M5.Display.height(), "scn");
    if (e != LOOK_OK || !s_img.cv) return false;
    s_img.cv->pushSprite(dst, 0, 0);
    return true;
}

void k85_look_wall_menu(K85WallSlot slot) {
    cfg_load();
    mkdir(WALL_DIR, 0755);
    image_menu((int)slot);
}

void k85_look_run_settings(void) {
    cfg_load();
    mkdir(WALL_DIR, 0755);
    while (true) {
        char l0[32], l1[32], l2[32], l3[32];
        snprintf(l0, sizeof(l0), "Wall: Lock %s", s_cfg.wall[0][0] ? "ON" : "off");
        snprintf(l1, sizeof(l1), "Wall: Main %s", s_cfg.wall[1][0] ? "ON" : "off");
        snprintf(l2, sizeof(l2), "Wall: System %s", s_cfg.wall[2][0] ? "ON" : "off");
        snprintf(l3, sizeof(l3), "Banner: %s", s_cfg.banner[0] ? "ON" : "off");
        const char *items[] = { l0, l1, l2, l3, "Back" };
        int idx = k85_run_list_menu("LOOK", items, 5, nullptr);
        if (idx < 0 || idx == 4) return;
        image_menu(idx);        // 0 lock, 1 main, 2 system, 3 banner
    }
}