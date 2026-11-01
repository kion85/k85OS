// K85FX1: lock screen scene engine (smooth particles, scenes, explosions).
// A frame is composed off-screen (wallpaper or gradient, particles, bursts, clock) and pushed to the display
// in one go, so nothing flickers; movement is time-based, so it stays smooth at any frame rate.
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "lock_fx.h"
#include "look.h"
#include "theme.h"
#include "config.h"
#include "common.h"
#include "list_menu.h"
#include "input.h"
#include "rtc_ntp.h"
#include "log.h"

#include "M5Unified.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <strings.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

#define FX_MAXP      96
#define FX_MAXB      72
#define FX_MAXSTARS  40
#define FX_BUILTIN   8
#define FX_DIR       "/littlefs/lock"
#define FX_CFG       "/littlefs/k85_lockfx.txt"

enum { MODE_DRIFT = 0, MODE_FALL, MODE_RISE, MODE_TWINKLE, MODE_RAIN, MODE_LIGHTS };
enum { SH_DOT = 0, SH_SQUARE, SH_STAR, SH_HEART, SH_SNOW, SH_LINE, SH_LETTER, SH_MIXED };
enum { ES_SPARKS = 0, ES_CONFETTI, ES_RING };
enum { TR_OFF = 0, TR_BUTTON, TR_SHAKE, TR_BOTH };

struct Scene {
    char name[24];
    uint32_t bg_top, bg_bot;
    char bg_image[48];
    uint8_t bg_dim;
    int count, stars;
    uint16_t shapes;                     // bit mask of SH_* the particles are picked from
    uint8_t mode;
    float size_min, size_max, speed_min, speed_max, gravity, sway;
    uint32_t colors[8]; int ncolors;
    uint8_t e_trigger, e_style;
    int e_count, e_life;
    float e_smin, e_smax, e_grav;
    uint32_t e_colors[6]; int e_ncolors;
};

struct P { float x, y, vx, vy, size, phase; uint32_t color; uint8_t shape; };
struct Bst { float x, y, vx, vy, life, maxlife; uint32_t color; uint8_t kind; };
struct Star { int16_t x, y; float phase, rate; };
struct FxCfg { char scene[48]; int shape_ovr, speed, density, expl, estyle, power; };

static const char *const BUILTIN_NAMES[FX_BUILTIN] = { "Classic", "Snow", "Stars", "Sparks", "Letters", "Lights", "Hearts", "Rain" };

static const uint32_t C_CLASSIC[] = { 0x88CCFF, 0xFFFFFF, 0xCC99FF, 0x99FFEE, 0xFFDD99 };
static const uint32_t C_SNOW[]    = { 0xFFFFFF, 0xCCE6FF, 0xE6F2FF };
static const uint32_t C_STARS[]   = { 0xFFFFFF, 0xCCDDFF, 0xFFF2CC };
static const uint32_t C_SPARKS[]  = { 0xFFAA00, 0xFF6600, 0xFFDD44, 0xFF3300 };
static const uint32_t C_GREEN[]   = { 0x00FF66, 0x00CC55 };
static const uint32_t C_LIGHTS[]  = { 0xFF66AA, 0x66CCFF, 0xFFCC66, 0x99FF99, 0xCC99FF };
static const uint32_t C_HEARTS[]  = { 0xFF5588, 0xFF88AA, 0xFFAACC, 0xFF3366 };
static const uint32_t C_RAIN[]    = { 0x6699FF, 0x99BBFF };
static const uint32_t C_FIRE[]    = { 0xFFAA00, 0xFF5500, 0xFFFF66 };
static const uint32_t C_ICE[]     = { 0xFFFFFF, 0x99CCFF, 0x66AAFF };
static const uint32_t C_PARTY[]   = { 0xFF4466, 0x44DDFF, 0xFFDD33, 0x66FF88, 0xCC66FF };
static const uint32_t C_GREENB[]  = { 0x66FF99, 0xCCFFCC, 0x00FF66 };

#define SETC(dst, n, arr) do { n = (int)(sizeof(arr) / sizeof(arr[0])); if (n > (int)(sizeof(dst) / sizeof(dst[0]))) n = (int)(sizeof(dst) / sizeof(dst[0])); memcpy(dst, arr, (size_t)n * sizeof(uint32_t)); } while (0)

static FxCfg s_cfg;
static bool s_cfg_loaded = false;
static Scene s_scene;
static P s_p[FX_MAXP];
static int s_np = 0;
static Bst s_b[FX_MAXB];
static int s_nb = 0;
static Star s_stars[FX_MAXSTARS];
static int s_nstars = 0;
static M5Canvas *s_fx = nullptr;
static M5Canvas *s_bg = nullptr;
static int s_w = 0, s_h = 0;
static int64_t s_last_us = 0;
static uint32_t s_last_shake_ms = 0;
static bool s_ring_on = false;
static float s_ring_x = 0, s_ring_y = 0, s_ring_t = 0;
static uint32_t s_ring_col = 0xFFFFFF;
static float s_spd = 1.0f;
static bool s_frozen = false;

// ---------------- small helpers ----------------
static float rndf(float a, float b) { return a + (b - a) * ((float)(esp_random() & 0xFFFF) / 65535.0f); }
static uint32_t hash2(uint32_t a, uint32_t b) { uint32_t h = a * 374761393u + b * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
static uint32_t scale_col(uint32_t c, int b) {
    if (b >= 255) return c & 0xFFFFFF;
    if (b < 0) b = 0;
    uint32_t r = ((c >> 16) & 0xFF) * (uint32_t)b / 255;
    uint32_t g = ((c >> 8) & 0xFF) * (uint32_t)b / 255;
    uint32_t bl = (c & 0xFF) * (uint32_t)b / 255;
    return (r << 16) | (g << 8) | bl;
}
static uint32_t lerp_col(uint32_t a, uint32_t b, int t256) {
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg2 = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * t256 / 256, g = ag + (bg2 - ag) * t256 / 256, bl = ab + (bb - ab) * t256 / 256;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

// ---------------- built-in scenes ----------------
static void scene_classic(Scene &s) {
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "Classic");
    s.bg_top = s.bg_bot = 0x000000;
    s.count = 14; s.stars = 25;
    s.shapes = 1u << SH_DOT;
    s.mode = MODE_DRIFT;
    s.size_min = 1.0f; s.size_max = 2.5f; s.speed_min = 6.0f; s.speed_max = 20.0f;
    SETC(s.colors, s.ncolors, C_CLASSIC);
    s.e_trigger = TR_BOTH; s.e_style = ES_SPARKS; s.e_count = 28; s.e_life = 900;
    s.e_smin = 30.0f; s.e_smax = 90.0f; s.e_grav = 40.0f;
    SETC(s.e_colors, s.e_ncolors, C_FIRE);
}

static void scene_builtin(int i, Scene &s) {
    scene_classic(s);
    if (i < 0 || i >= FX_BUILTIN) return;
    snprintf(s.name, sizeof(s.name), "%s", BUILTIN_NAMES[i]);
    switch (i) {
        case 1:   // Snow
            s.bg_top = 0x000510; s.bg_bot = 0x103050; s.stars = 0; s.count = 40;
            s.shapes = (1u << SH_DOT) | (1u << SH_SNOW); s.mode = MODE_FALL; s.sway = 10.0f;
            s.size_min = 1.0f; s.size_max = 2.6f; s.speed_min = 14.0f; s.speed_max = 34.0f;
            SETC(s.colors, s.ncolors, C_SNOW);
            s.e_trigger = TR_BUTTON; SETC(s.e_colors, s.e_ncolors, C_ICE);
            break;
        case 2:   // Stars
            s.bg_top = 0x000000; s.bg_bot = 0x050520; s.stars = 0; s.count = 38;
            s.shapes = (1u << SH_DOT) | (1u << SH_STAR); s.mode = MODE_TWINKLE;
            s.size_min = 1.0f; s.size_max = 2.0f; s.speed_min = 6.0f; s.speed_max = 20.0f;
            SETC(s.colors, s.ncolors, C_STARS);
            s.e_style = ES_RING; SETC(s.e_colors, s.e_ncolors, C_ICE);
            break;
        case 3:   // Sparks
            s.bg_top = 0x080000; s.bg_bot = 0x200800; s.stars = 0; s.count = 40;
            s.shapes = (1u << SH_LINE) | (1u << SH_DOT); s.mode = MODE_RISE; s.sway = 6.0f;
            s.size_min = 1.0f; s.size_max = 2.0f; s.speed_min = 30.0f; s.speed_max = 80.0f;
            SETC(s.colors, s.ncolors, C_SPARKS);
            break;
        case 4:   // Letters (matrix rain)
            s.bg_top = 0x000000; s.bg_bot = 0x001400; s.stars = 0; s.count = 30;
            s.shapes = 1u << SH_LETTER; s.mode = MODE_RAIN;
            s.size_min = 1.0f; s.size_max = 1.0f; s.speed_min = 25.0f; s.speed_max = 70.0f;
            SETC(s.colors, s.ncolors, C_GREEN);
            s.e_trigger = TR_BUTTON; SETC(s.e_colors, s.e_ncolors, C_GREENB);
            break;
        case 5:   // Lights (bokeh)
            s.bg_top = 0x050010; s.bg_bot = 0x1A0630; s.stars = 0; s.count = 14;
            s.shapes = 1u << SH_DOT; s.mode = MODE_LIGHTS;
            s.size_min = 6.0f; s.size_max = 14.0f; s.speed_min = 4.0f; s.speed_max = 12.0f;
            SETC(s.colors, s.ncolors, C_LIGHTS);
            s.e_trigger = TR_BUTTON; s.e_style = ES_RING;
            break;
        case 6:   // Hearts
            s.bg_top = 0x100008; s.bg_bot = 0x300818; s.stars = 0; s.count = 18;
            s.shapes = 1u << SH_HEART; s.mode = MODE_RISE; s.sway = 8.0f;
            s.size_min = 2.0f; s.size_max = 4.0f; s.speed_min = 14.0f; s.speed_max = 32.0f;
            SETC(s.colors, s.ncolors, C_HEARTS);
            s.e_trigger = TR_BUTTON; s.e_style = ES_CONFETTI; SETC(s.e_colors, s.e_ncolors, C_HEARTS);
            break;
        case 7:   // Rain
            s.bg_top = 0x000008; s.bg_bot = 0x061428; s.stars = 0; s.count = 60;
            s.shapes = 1u << SH_LINE; s.mode = MODE_FALL;
            s.size_min = 1.0f; s.size_max = 1.0f; s.speed_min = 160.0f; s.speed_max = 260.0f;
            SETC(s.colors, s.ncolors, C_RAIN);
            s.e_trigger = TR_SHAKE; s.e_style = ES_RING; SETC(s.e_colors, s.e_ncolors, C_ICE);
            break;
        default: break;
    }
    (void)C_PARTY;
}

// ---------------- settings file ----------------
static void cfg_load(void) {
    if (s_cfg_loaded) return;
    s_cfg_loaded = true;
    memset(&s_cfg, 0, sizeof(s_cfg));
    snprintf(s_cfg.scene, sizeof(s_cfg.scene), "Classic");
    s_cfg.shape_ovr = -1; s_cfg.speed = 1; s_cfg.density = 1; s_cfg.expl = -1; s_cfg.estyle = -1; s_cfg.power = 1;
    FILE *f = fopen(FX_CFG, "r");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "scene")) snprintf(s_cfg.scene, sizeof(s_cfg.scene), "%.47s", v);
        else if (!strcmp(k, "shape")) { int x = atoi(v); s_cfg.shape_ovr = x < -1 ? -1 : (x > 7 ? 7 : x); }
        else if (!strcmp(k, "speed")) { int x = atoi(v); s_cfg.speed = x < 0 ? 0 : (x > 2 ? 2 : x); }
        else if (!strcmp(k, "density")) { int x = atoi(v); s_cfg.density = x < 0 ? 0 : (x > 2 ? 2 : x); }
        else if (!strcmp(k, "expl")) { int x = atoi(v); s_cfg.expl = x < -1 ? -1 : (x > 3 ? 3 : x); }
        else if (!strcmp(k, "estyle")) { int x = atoi(v); s_cfg.estyle = x < -1 ? -1 : (x > 2 ? 2 : x); }
        else if (!strcmp(k, "power")) { int x = atoi(v); s_cfg.power = x < 0 ? 0 : (x > 2 ? 2 : x); }
    }
    fclose(f);
}

static void cfg_save(void) {
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%s.tmp", FX_CFG);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "scene=%s\nshape=%d\nspeed=%d\ndensity=%d\nexpl=%d\nestyle=%d\n",
            s_cfg.scene, s_cfg.shape_ovr, s_cfg.speed, s_cfg.density, s_cfg.expl, s_cfg.estyle);
    fprintf(f, "power=%d\n", s_cfg.power);
    bool ok = (ferror(f) == 0);
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return; }
    remove(FX_CFG);
    rename(tmp, FX_CFG);
}

// ---------------- scene files (JSON) ----------------
static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(it) ? (float)it->valuedouble : def;
}
static void jrange(const cJSON *o, const char *k, float *a, float *b) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(it)) { *a = *b = (float)it->valuedouble; return; }
    if (cJSON_IsArray(it) && cJSON_GetArraySize(it) >= 2) {
        const cJSON *x = cJSON_GetArrayItem(it, 0), *y = cJSON_GetArrayItem(it, 1);
        if (cJSON_IsNumber(x) && cJSON_IsNumber(y)) { *a = (float)x->valuedouble; *b = (float)y->valuedouble; }
    }
    if (*b < *a) { float t = *a; *a = *b; *b = t; }
}
static uint32_t jcolor(const cJSON *it, uint32_t def) {
    if (cJSON_IsNumber(it)) return (uint32_t)it->valuedouble & 0xFFFFFF;
    if (cJSON_IsString(it) && it->valuestring) {
        const char *p = it->valuestring;
        if (*p == '#') p++;
        return (uint32_t)strtoul(p, nullptr, 16) & 0xFFFFFF;
    }
    return def;
}
static int jcolors(const cJSON *o, const char *k, uint32_t *out, int max) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(arr)) return 0;
    int n = 0, cnt = cJSON_GetArraySize(arr);
    for (int i = 0; i < cnt && n < max; i++) out[n++] = jcolor(cJSON_GetArrayItem(arr, i), 0xFFFFFF);
    return n;
}
static int shape_from_name(const char *s) {
    if (!strcasecmp(s, "dot") || !strcasecmp(s, "circle")) return SH_DOT;
    if (!strcasecmp(s, "square")) return SH_SQUARE;
    if (!strcasecmp(s, "star")) return SH_STAR;
    if (!strcasecmp(s, "heart")) return SH_HEART;
    if (!strcasecmp(s, "snow") || !strcasecmp(s, "snowflake")) return SH_SNOW;
    if (!strcasecmp(s, "line") || !strcasecmp(s, "streak")) return SH_LINE;
    if (!strcasecmp(s, "letter") || !strcasecmp(s, "char")) return SH_LETTER;
    return -1;
}
static uint16_t jshapes(const cJSON *o, const char *k, uint16_t def) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    uint16_t m = 0;
    if (cJSON_IsString(it) && it->valuestring) {
        int s = shape_from_name(it->valuestring);
        if (s >= 0) m |= (uint16_t)(1u << s);
    } else if (cJSON_IsArray(it)) {
        int cnt = cJSON_GetArraySize(it);
        for (int i = 0; i < cnt; i++) {
            const cJSON *e = cJSON_GetArrayItem(it, i);
            if (cJSON_IsString(e) && e->valuestring) { int s = shape_from_name(e->valuestring); if (s >= 0) m |= (uint16_t)(1u << s); }
        }
    }
    return m ? m : def;
}
static uint8_t jenum(const cJSON *o, const char *k, const char *const *names, int n, uint8_t def) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(it) || !it->valuestring) return def;
    for (int i = 0; i < n; i++) if (!strcasecmp(it->valuestring, names[i])) return (uint8_t)i;
    return def;
}

static bool load_json_scene(const char *file, Scene &s) {
    char path[96];
    snprintf(path, sizeof(path), "%s/%.47s", FX_DIR, file);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16384) { fclose(f); return false; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    static const char *const modes[] = { "drift", "fall", "rise", "twinkle", "rain", "lights" };
    static const char *const triggers[] = { "off", "button", "shake", "both" };
    static const char *const styles[] = { "sparks", "confetti", "ring" };

    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(nm) && nm->valuestring) snprintf(s.name, sizeof(s.name), "%.23s", nm->valuestring);

    const cJSON *bg = cJSON_GetObjectItemCaseSensitive(root, "background");
    if (cJSON_IsObject(bg)) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(bg, "color");
        if (c) s.bg_top = s.bg_bot = jcolor(c, s.bg_top);
        const cJSON *g = cJSON_GetObjectItemCaseSensitive(bg, "gradient");
        if (cJSON_IsArray(g) && cJSON_GetArraySize(g) >= 2) {
            s.bg_top = jcolor(cJSON_GetArrayItem(g, 0), s.bg_top);
            s.bg_bot = jcolor(cJSON_GetArrayItem(g, 1), s.bg_bot);
        }
        const cJSON *im = cJSON_GetObjectItemCaseSensitive(bg, "image");
        if (cJSON_IsString(im) && im->valuestring) snprintf(s.bg_image, sizeof(s.bg_image), "%.47s", im->valuestring);
        float dim = jnum(bg, "dim", (float)s.bg_dim);
        s.bg_dim = (uint8_t)(dim < 0 ? 0 : (dim > 90 ? 90 : dim));
    }
    const cJSON *pa = cJSON_GetObjectItemCaseSensitive(root, "particles");
    if (cJSON_IsObject(pa)) {
        s.count = (int)jnum(pa, "count", (float)s.count);
        s.stars = (int)jnum(pa, "stars", (float)s.stars);
        s.shapes = jshapes(pa, "shape", s.shapes);
        s.mode = jenum(pa, "mode", modes, 6, s.mode);
        jrange(pa, "size", &s.size_min, &s.size_max);
        jrange(pa, "speed", &s.speed_min, &s.speed_max);
        s.gravity = jnum(pa, "gravity", s.gravity);
        s.sway = jnum(pa, "sway", s.sway);
        int nc = jcolors(pa, "colors", s.colors, 8);
        if (nc > 0) s.ncolors = nc;
    }
    const cJSON *ex = cJSON_GetObjectItemCaseSensitive(root, "explosion");
    if (cJSON_IsObject(ex)) {
        s.e_trigger = jenum(ex, "trigger", triggers, 4, s.e_trigger);
        s.e_style = jenum(ex, "style", styles, 3, s.e_style);
        s.e_count = (int)jnum(ex, "count", (float)s.e_count);
        s.e_life = (int)jnum(ex, "life_ms", (float)s.e_life);
        jrange(ex, "speed", &s.e_smin, &s.e_smax);
        s.e_grav = jnum(ex, "gravity", s.e_grav);
        int nc = jcolors(ex, "colors", s.e_colors, 6);
        if (nc > 0) s.e_ncolors = nc;
    }
    cJSON_Delete(root);
    if (s.count < 0) s.count = 0;
    if (s.count > FX_MAXP) s.count = FX_MAXP;
    if (s.stars < 0) s.stars = 0;
    if (s.stars > FX_MAXSTARS) s.stars = FX_MAXSTARS;
    if (s.e_count < 0) s.e_count = 0;
    if (s.e_count > FX_MAXB) s.e_count = FX_MAXB;
    if (s.e_life < 200) s.e_life = 200;
    if (s.ncolors <= 0) { s.ncolors = 1; s.colors[0] = 0xFFFFFF; }
    if (s.e_ncolors <= 0) { s.e_ncolors = 1; s.e_colors[0] = 0xFFAA00; }
    return true;
}

static void scene_load_current(Scene &s) {
    scene_classic(s);
    size_t l = strlen(s_cfg.scene);
    if (l > 5 && !strcasecmp(s_cfg.scene + l - 5, ".json")) {
        if (!load_json_scene(s_cfg.scene, s)) scene_classic(s);
        return;
    }
    for (int i = 0; i < FX_BUILTIN; i++) if (!strcmp(BUILTIN_NAMES[i], s_cfg.scene)) { scene_builtin(i, s); return; }
}

// ---------------- effective settings ----------------
static int eff_trigger(void) { return s_cfg.expl >= 0 ? s_cfg.expl : s_scene.e_trigger; }
static int eff_style(void) { return s_cfg.estyle >= 0 ? s_cfg.estyle : s_scene.e_style; }
static uint16_t eff_shapes(void) {
    if (s_cfg.shape_ovr < 0) return s_scene.shapes;
    if (s_cfg.shape_ovr == SH_MIXED) return (uint16_t)((1u << SH_DOT) | (1u << SH_SQUARE) | (1u << SH_STAR) | (1u << SH_HEART) | (1u << SH_SNOW));
    return (uint16_t)(1u << s_cfg.shape_ovr);
}
static uint8_t pick_shape(uint16_t mask) {
    uint8_t opts[8];
    int n = 0;
    for (int i = 0; i < 7; i++) if (mask & (1u << i)) opts[n++] = (uint8_t)i;
    if (n == 0) return SH_DOT;
    return opts[esp_random() % (uint32_t)n];
}
static uint32_t pick_ecolor(void) { return s_scene.e_colors[esp_random() % (uint32_t)(s_scene.e_ncolors > 0 ? s_scene.e_ncolors : 1)]; }

// ---------------- particles ----------------
static void spawn(P &p, bool initial) {
    float w = (float)s_w, h = (float)s_h;
    p.shape = pick_shape(eff_shapes());
    p.size = rndf(s_scene.size_min, s_scene.size_max);
    p.color = s_scene.ncolors > 0 ? s_scene.colors[esp_random() % (uint32_t)s_scene.ncolors] : 0xFFFFFF;
    p.phase = rndf(0.0f, 6.2831853f);
    float sp = rndf(s_scene.speed_min, s_scene.speed_max);
    if (s_scene.mode != MODE_RAIN) p.x = rndf(0.0f, w);
    switch (s_scene.mode) {
        case MODE_FALL:    p.y = initial ? rndf(0.0f, h) : -rndf(2.0f, 24.0f); p.vx = 0; p.vy = sp; break;
        case MODE_RISE:    p.y = initial ? rndf(0.0f, h) : h + rndf(2.0f, 24.0f); p.vx = 0; p.vy = -sp; break;
        case MODE_TWINKLE: p.y = rndf(0.0f, h); p.vx = sp * 0.15f; p.vy = 0; break;          // vx = twinkle rate
        case MODE_LIGHTS:  p.y = rndf(0.0f, h); p.vx = rndf(-sp, sp); p.vy = rndf(-sp, sp); break;
        case MODE_RAIN:    p.y = initial ? rndf(-h, h) : -rndf(8.0f, h * 0.6f); p.vx = 0; p.vy = sp; break;
        default: { float a = rndf(0.0f, 6.2831853f); p.y = rndf(0.0f, h); p.vx = cosf(a) * sp; p.vy = sinf(a) * sp; break; }
    }
}

static void init_particles(void) {
    float dmul = s_cfg.density == 0 ? 0.5f : (s_cfg.density == 2 ? 1.6f : 1.0f);
    int n = (int)((float)s_scene.count * dmul + 0.5f);
    if (s_scene.mode == MODE_RAIN) n = s_w / 8;
    if (n < 0) n = 0;
    if (n > FX_MAXP) n = FX_MAXP;
    s_np = n;
    for (int i = 0; i < s_np; i++) {
        spawn(s_p[i], true);
        if (s_scene.mode == MODE_RAIN) s_p[i].x = (float)(i * 8 + 1);
    }
    s_nstars = s_scene.stars > FX_MAXSTARS ? FX_MAXSTARS : s_scene.stars;
    for (int i = 0; i < s_nstars; i++) {
        s_stars[i].x = (int16_t)(esp_random() % (uint32_t)s_w);
        s_stars[i].y = (int16_t)(esp_random() % (uint32_t)s_h);
        s_stars[i].phase = rndf(0.0f, 6.2831853f);
        s_stars[i].rate = rndf(1.0f, 3.0f);
    }
    s_nb = 0;
    s_ring_on = false;
}

static void update(float dt) {
    float w = (float)s_w, h = (float)s_h;
    for (int i = 0; i < s_np; i++) {
        P &p = s_p[i];
        switch (s_scene.mode) {
            case MODE_DRIFT:
                p.x += p.vx * dt * s_spd; p.y += p.vy * dt * s_spd;
                p.vy += s_scene.gravity * dt;
                if (p.x < 0) { p.x = 0; p.vx = fabsf(p.vx); } else if (p.x > w) { p.x = w; p.vx = -fabsf(p.vx); }
                if (p.y < 0) { p.y = 0; p.vy = fabsf(p.vy); } else if (p.y > h) { p.y = h; p.vy = -fabsf(p.vy); }
                break;
            case MODE_FALL:
            case MODE_RISE:
                p.y += p.vy * dt * s_spd;
                p.phase += dt * 1.6f;
                p.x += sinf(p.phase) * s_scene.sway * dt * s_spd;
                if (p.x < -6.0f) p.x += w + 12.0f; else if (p.x > w + 6.0f) p.x -= w + 12.0f;
                if ((s_scene.mode == MODE_FALL && p.y > h + 8.0f) || (s_scene.mode == MODE_RISE && p.y < -8.0f)) spawn(p, false);
                break;
            case MODE_TWINKLE:
                p.phase += p.vx * dt * s_spd;
                break;
            case MODE_LIGHTS:
                p.x += p.vx * dt * s_spd; p.y += p.vy * dt * s_spd;
                p.phase += dt * 0.9f * s_spd;
                if (p.x < -p.size) p.x = w + p.size; else if (p.x > w + p.size) p.x = -p.size;
                if (p.y < -p.size) p.y = h + p.size; else if (p.y > h + p.size) p.y = -p.size;
                break;
            case MODE_RAIN:
                p.y += p.vy * dt * s_spd;
                if (p.y - 72.0f > h) { float x = p.x; spawn(p, false); p.x = x; }
                break;
        }
    }
    for (int i = 0; i < s_nstars; i++) s_stars[i].phase += s_stars[i].rate * dt;

    for (int i = s_nb - 1; i >= 0; i--) {
        Bst &b = s_b[i];
        b.life -= dt * 1000.0f;
        if (b.life <= 0.0f) { s_b[i] = s_b[--s_nb]; continue; }
        b.vy += s_scene.e_grav * (b.kind ? 2.2f : 1.0f) * dt;
        float drag = 1.0f - 1.3f * dt;
        if (drag < 0.0f) drag = 0.0f;
        b.vx *= drag; b.vy *= drag;
        b.x += b.vx * dt; b.y += b.vy * dt;
    }
    if (s_ring_on) {
        s_ring_t += dt / ((float)s_scene.e_life / 1000.0f * 0.8f);
        if (s_ring_t >= 1.0f) s_ring_on = false;
    }
}

static void spawn_burst(float cx, float cy) {
    int style = eff_style();
    if (style == ES_RING) { s_ring_on = true; s_ring_x = cx; s_ring_y = cy; s_ring_t = 0.0f; s_ring_col = pick_ecolor(); }
    int n = s_scene.e_count;
    if (style == ES_RING) n /= 3;
    for (int i = 0; i < n && s_nb < FX_MAXB; i++) {
        Bst &b = s_b[s_nb++];
        float a = rndf(0.0f, 6.2831853f);
        float sp = rndf(s_scene.e_smin, s_scene.e_smax);
        b.x = cx; b.y = cy; b.vx = cosf(a) * sp; b.vy = sinf(a) * sp;
        b.maxlife = b.life = (float)s_scene.e_life * rndf(0.6f, 1.0f);
        b.color = pick_ecolor();
        b.kind = (style == ES_CONFETTI) ? 1 : 0;
    }
}

// ---------------- drawing ----------------
static void draw_rain_column(const P &p, uint32_t base) {
    M5Canvas &g = *s_fx;
    uint32_t tick = (uint32_t)(esp_timer_get_time() / 160000);
    g.setTextSize(1);
    for (int k = 0; k < 9; k++) {
        int y = (int)p.y - k * 8;
        if (y < -8 || y > s_h) continue;
        int b = 255 - k * 28;
        if (b < 20) b = 20;
        uint32_t c = (k == 0) ? 0xCCFFCC : scale_col(base, b);
        char ch[2] = { (char)(33 + hash2((uint32_t)p.x * 31u + (uint32_t)(y + 64) / 8u, tick + (uint32_t)k) % 94u), 0 };
        g.setTextColor(c);
        g.setCursor((int)p.x, y);
        g.print(ch);
    }
}

static void draw_particles(void) {
    M5Canvas &g = *s_fx;
    bool override = g_config.lock_particle_color != 0xFFFFFFFF;
    float h = (float)s_h;
    for (int i = 0; i < s_np; i++) {
        const P &p = s_p[i];
        uint32_t base = override ? (g_config.lock_particle_color & 0xFFFFFF) : p.color;
        if (s_scene.mode == MODE_RAIN) { draw_rain_column(p, base); continue; }
        int b = 255;
        switch (s_scene.mode) {
            case MODE_RISE: { float t = p.y / h; if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f; b = (int)(80.0f + 175.0f * t); break; }
            case MODE_TWINKLE: b = (int)(60.0f + 195.0f * (0.5f + 0.5f * sinf(p.phase))); break;
            case MODE_LIGHTS: b = (int)(150.0f + 70.0f * sinf(p.phase)); break;
            default: break;
        }
        uint32_t c = scale_col(base, b);
        int x = (int)p.x, y = (int)p.y;
        float sz = p.size;
        if (s_scene.mode == MODE_LIGHTS) {
            float r = sz * (1.0f + 0.15f * sinf(p.phase * 1.3f));
            g.fillCircle(x, y, (int)r, scale_col(base, (int)((float)b * 0.35f)));
            g.fillCircle(x, y, (int)(r * 0.68f), scale_col(base, (int)((float)b * 0.6f)));
            g.fillCircle(x, y, (int)(r * 0.36f), c);
            continue;
        }
        switch (p.shape) {
            case SH_DOT: { int r = (int)(sz + 0.5f); if (r < 1) r = 1; g.fillCircle(x, y, r, c); break; }
            case SH_SQUARE: { int r = (int)(sz + 0.5f); if (r < 1) r = 1; g.fillRect(x - r, y - r, r * 2, r * 2, c); break; }
            case SH_STAR: {
                int r = (int)(sz * 2.0f + 0.5f); if (r < 2) r = 2;
                g.drawFastHLine(x - r, y, r * 2 + 1, c);
                g.drawFastVLine(x, y - r, r * 2 + 1, c);
                g.drawPixel(x, y, 0xFFFFFF);
                break;
            }
            case SH_HEART: {
                int r = (int)(sz * 1.5f + 0.5f); if (r < 3) r = 3;
                g.fillCircle(x - r / 2, y - r / 2, r / 2 + 1, c);
                g.fillCircle(x + r / 2, y - r / 2, r / 2 + 1, c);
                g.fillTriangle(x - r, y - r / 4, x + r, y - r / 4, x, y + r, c);
                break;
            }
            case SH_SNOW: {
                int r = (int)(sz * 1.6f + 0.5f); if (r < 2) r = 2;
                int d = r * 7 / 10;
                g.drawFastHLine(x - r, y, r * 2 + 1, c);
                g.drawFastVLine(x, y - r, r * 2 + 1, c);
                g.drawLine(x - d, y - d, x + d, y + d, c);
                g.drawLine(x - d, y + d, x + d, y - d, c);
                break;
            }
            case SH_LINE: {
                int x2 = (int)(p.x - p.vx * 0.045f * s_spd), y2 = (int)(p.y - p.vy * 0.045f * s_spd);
                g.drawLine(x, y, x2, y2, c);
                break;
            }
            case SH_LETTER: {
                char ch[2] = { (char)(33 + hash2((uint32_t)i, (uint32_t)(p.phase * 10.0f) + (uint32_t)(esp_timer_get_time() / 400000)) % 94u), 0 };
                g.setTextSize(1);
                g.setTextColor(c);
                g.setCursor(x, y);
                g.print(ch);
                break;
            }
            default: break;
        }
    }
}

static void draw_bursts(void) {
    M5Canvas &g = *s_fx;
    for (int i = 0; i < s_nb; i++) {
        const Bst &b = s_b[i];
        float f = b.life / b.maxlife;
        int br = (int)(255.0f * f);
        uint32_t c = scale_col(b.color, br);
        if (b.kind == 0) {
            g.drawLine((int)b.x, (int)b.y, (int)(b.x - b.vx * 0.03f), (int)(b.y - b.vy * 0.03f), c);
            g.drawPixel((int)b.x, (int)b.y, scale_col(0xFFFFFF, br));
        } else {
            g.fillRect((int)b.x - 1, (int)b.y - 1, 3, 2, c);
        }
    }
    if (s_ring_on) {
        float big = (float)(s_w < s_h ? s_h : s_w) * 0.55f;
        int r = (int)(s_ring_t * big);
        int br = (int)(255.0f * (1.0f - s_ring_t));
        uint32_t c = scale_col(s_ring_col, br);
        g.drawCircle((int)s_ring_x, (int)s_ring_y, r, c);
        if (r > 1) g.drawCircle((int)s_ring_x, (int)s_ring_y, r - 1, scale_col(s_ring_col, br / 2));
    }
}

static void draw_stars(void) {
    M5Canvas &g = *s_fx;
    for (int i = 0; i < s_nstars; i++) {
        float t = 0.5f + 0.5f * sinf(s_stars[i].phase);
        int v = (int)(40.0f + 215.0f * t);
        g.drawPixel(s_stars[i].x, s_stars[i].y, ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v);
    }
}

static void draw_texts(const char *tm, const char *date, const char *name, const char *info) {
    M5Canvas &g = *s_fx;
    int w = s_w, h = s_h;
    g.setTextSize(3);
    g.setTextColor(0xFFFFFF);
    g.setCursor((w - (int)strlen(tm) * 18) / 2, h / 2 - 35);
    g.print(tm);
    g.setTextSize(1);
    if (date) {
        g.setTextColor(0xBBBBBB);
        g.setCursor((w - (int)strlen(date) * 6) / 2, h / 2 - 18);
        g.print(date);
    }
    g.setTextColor(0xBBBBBB);
    g.setCursor((w - (int)strlen(name) * 6) / 2, h / 2 + 5);
    g.print(name);
    g.setTextColor(0xCCCCCC);
    g.setCursor((w - (int)strlen(info) * 6) / 2, h / 2 + 18);
    g.print(info);
    g.setTextColor(0x909090);
    g.setCursor(10, h - 14);
    g.print("A+B hold to unlock");
}

// ---------------- buffers ----------------
static void free_canvases(void) {
    if (s_fx) { s_fx->deleteSprite(); delete s_fx; s_fx = nullptr; }
    if (s_bg) { s_bg->deleteSprite(); delete s_bg; s_bg = nullptr; }
}

static void build_bg(void) {
    bool have = k85_wall_push(K85_WALL_LOCK, s_bg);
    if (!have && s_scene.bg_image[0]) have = k85_image_push(s_scene.bg_image, s_scene.bg_dim, s_bg);
    if (have) return;
    if (s_scene.bg_top == s_scene.bg_bot) { s_bg->fillScreen(s_scene.bg_top); return; }
    for (int y = 0; y < s_h; y++) s_bg->drawFastHLine(0, y, s_w, lerp_col(s_scene.bg_top, s_scene.bg_bot, y * 256 / (s_h > 1 ? s_h - 1 : 1)));
}

static bool ensure_ready(void) {
    int w = M5.Display.width(), h = M5.Display.height();
    if (s_fx && s_bg && s_w == w && s_h == h) return true;
    free_canvases();
    s_w = w; s_h = h;
    s_fx = new M5Canvas(&M5.Display);
    s_bg = new M5Canvas(&M5.Display);
    if (!s_fx || !s_bg) { free_canvases(); return false; }
    s_fx->setColorDepth(16); s_fx->setPsram(true);
    s_bg->setColorDepth(16); s_bg->setPsram(true);
    if (!s_fx->createSprite(w, h) || !s_bg->createSprite(w, h)) { free_canvases(); return false; }
    cfg_load();
    scene_load_current(s_scene);
    s_spd = s_cfg.speed == 0 ? 0.5f : (s_cfg.speed == 2 ? 1.8f : 1.0f);
    build_bg();
    init_particles();
    s_last_us = 0;
    return true;
}

// ---------------- public API ----------------
void k85_lockfx_begin(void) {
    free_canvases();
    s_last_us = 0;
}

void k85_lockfx_end(void) {
    free_canvases();
    s_nb = 0;
    s_ring_on = false;
}

void k85_lockfx_button(void) {
    if (!s_fx) return;
    int t = eff_trigger();
    if (t == TR_BUTTON || t == TR_BOTH) spawn_burst((float)s_w * 0.5f, (float)s_h * 0.5f);
}

void k85_lockfx_frame(const char *tm, const char *date, const char *name, const char *info) {
    if (!ensure_ready()) {      // no memory for the buffers: plain fallback, the lock screen must still work
        M5.Display.fillScreen(0x000000);
        M5.Display.setTextSize(3);
        M5.Display.setTextColor(0xFFFFFF, 0x000000);
        M5.Display.setCursor((M5.Display.width() - (int)strlen(tm) * 18) / 2, M5.Display.height() / 2 - 35);
        M5.Display.print(tm);
        return;
    }
    int64_t now_us = esp_timer_get_time();
    uint32_t now_ms = (uint32_t)(now_us / 1000);
    float dt = s_last_us ? (float)(now_us - s_last_us) / 1e6f : 0.033f;
    if (dt < 0.004f) dt = 0.004f;
    if (dt > 0.1f) dt = 0.1f;
    s_last_us = now_us;

    int trig = eff_trigger();
    if (trig == TR_SHAKE || trig == TR_BOTH) {
        float ax = 0.0f, ay = 0.0f, az = 1.0f;
        M5.Imu.getAccel(&ax, &ay, &az);
        float m = sqrtf(ax * ax + ay * ay + az * az);
        if (fabsf(m - 1.0f) > 0.9f && (now_ms - s_last_shake_ms) > 600) {
            s_last_shake_ms = now_ms;
            spawn_burst((float)s_w * 0.5f, (float)s_h * 0.5f);
        }
    }

    if (!s_frozen || s_nb > 0 || s_ring_on) update(dt);
    s_bg->pushSprite(s_fx, 0, 0);
    draw_stars();
    draw_particles();
    draw_bursts();
    draw_texts(tm, date, name, info);
    s_fx->pushSprite(&M5.Display, 0, 0);
}

// ---------------- settings menu ----------------
static const uint32_t COLOR_PRESETS[] = { 0xFFFFFFFF, 0xFFFFFF, 0xFF6699, 0x66FFCC, 0xFFCC66, 0x9966FF, 0x00FFFF };
#define COLOR_PRESET_COUNT (int)(sizeof(COLOR_PRESETS) / sizeof(COLOR_PRESETS[0]))

static const char *EXAMPLE_JSON = R"JSON({
  "name": "My scene",
  "background": { "gradient": ["#000010", "#102040"], "image": "", "dim": 40 },
  "particles": { "count": 40, "shape": ["dot", "star"], "size": [1, 3], "speed": [10, 40],
                 "mode": "rise", "sway": 6, "colors": ["#FFFFFF", "#88CCFF"], "stars": 20 },
  "explosion": { "trigger": "both", "style": "sparks", "count": 30, "speed": [40, 120],
                 "gravity": 60, "life_ms": 900, "colors": ["#FFAA00", "#FF4400"] }
}
)JSON";

static const char *README_TXT =
    "Lock screen scenes. Put your own .json files in this folder and pick one in\r\n"
    "Settings -> Lock screen -> Scene. Copy example.json and edit it.\r\n\r\n"
    "background.color / gradient[2] : colors as \"#RRGGBB\"\r\n"
    "background.image               : file name from /wall (jpg, png, bmp), background.dim = 0..90\r\n"
    "particles.count                : 0..96     stars: 0..40 (twinkling dots)\r\n"
    "particles.shape                : dot square star heart snow line letter (one or a list)\r\n"
    "particles.mode                 : drift fall rise twinkle rain lights\r\n"
    "particles.size / speed         : [min, max]   speed in pixels per second\r\n"
    "particles.sway / gravity       : sideways wobble / pull (drift mode)\r\n"
    "particles.colors               : list of colors\r\n"
    "explosion.trigger              : off button shake both\r\n"
    "explosion.style                : sparks confetti ring\r\n"
    "explosion.count / speed / gravity / life_ms / colors\r\n"
    "Missing keys keep the Classic scene values.\r\n";

static void write_templates(void) {
    mkdir(FX_DIR, 0755);
    char p[64];
    snprintf(p, sizeof(p), "%s/example.json", FX_DIR);
    struct stat sb;
    if (stat(p, &sb) != 0) { FILE *f = fopen(p, "w"); if (f) { fputs(EXAMPLE_JSON, f); fclose(f); } }
    snprintf(p, sizeof(p), "%s/README.txt", FX_DIR);
    if (stat(p, &sb) != 0) { FILE *f = fopen(p, "w"); if (f) { fputs(README_TXT, f); fclose(f); } }
}

static int list_scene_files(char names[][48], int max) {
    DIR *d = opendir(FX_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && n < max) {
        size_t l = strlen(e->d_name);
        if (l < 6 || l >= 48 || strcasecmp(e->d_name + l - 5, ".json") != 0) continue;
        snprintf(names[n++], 48, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

static void choose_scene(void) {
    static char files[16][48];
    static char lbl[FX_BUILTIN + 16][24];
    int nf = list_scene_files(files, 16);
    const char *items[FX_BUILTIN + 16 + 1];
    int n = 0;
    for (int i = 0; i < FX_BUILTIN; i++) { snprintf(lbl[n], sizeof(lbl[n]), "%s", BUILTIN_NAMES[i]); items[n] = lbl[n]; n++; }
    for (int i = 0; i < nf; i++) { snprintf(lbl[n], sizeof(lbl[n]), "%.20s", files[i]); items[n] = lbl[n]; n++; }
    items[n] = "Back";
    int idx = k85_run_list_menu("SCENE", items, n + 1, nullptr);
    if (idx < 0 || idx >= n) return;
    if (idx < FX_BUILTIN) snprintf(s_cfg.scene, sizeof(s_cfg.scene), "%s", BUILTIN_NAMES[idx]);
    else snprintf(s_cfg.scene, sizeof(s_cfg.scene), "%s", files[idx - FX_BUILTIN]);
    cfg_save();
}

static void preview(void) {
    k85_lockfx_begin();
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        if (k85_btn_b_pressed() || k85_btn_a_pressed()) k85_lockfx_button();
        k85_lockfx_frame(k85_get_time_str(), nullptr, "preview", "tap = burst, shake it");
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    k85_lockfx_end();
}

void k85_lockfx_settings_menu(void) {
    cfg_load();
    write_templates();
    static const char *shape_names[] = { "Scene", "Circle", "Square", "Star", "Heart", "Snow", "Line", "Letter", "Mixed" };
    static const char *speed_names[] = { "Slow", "Medium", "Fast" };
    static const char *dens_names[] = { "Low", "Medium", "High" };
    static const char *expl_names[] = { "Scene", "Off", "Button", "Shake", "Both" };
    static const char *style_names[] = { "Scene", "Sparks", "Confetti", "Ring" };
    static const char *power_names[] = { "Saver", "Balanced", "Smooth" };
    while (true) {
        int ci = 0;
        for (int i = 0; i < COLOR_PRESET_COUNT; i++) if (COLOR_PRESETS[i] == g_config.lock_particle_color) ci = i;
        char l0[40], l1[32], l2[32], l3[32], l4[32], l5[32], l6[32], l7[32];
        snprintf(l0, sizeof(l0), "Scene: %.24s", s_cfg.scene);
        snprintf(l1, sizeof(l1), "Shape: %s", shape_names[s_cfg.shape_ovr + 1]);
        snprintf(l2, sizeof(l2), "Color: %s", ci == 0 ? "Auto" : "Custom");
        snprintf(l3, sizeof(l3), "Speed: %s", speed_names[s_cfg.speed]);
        snprintf(l4, sizeof(l4), "Density: %s", dens_names[s_cfg.density]);
        snprintf(l5, sizeof(l5), "Explosion: %s", expl_names[s_cfg.expl + 1]);
        snprintf(l6, sizeof(l6), "Burst: %s", style_names[s_cfg.estyle + 1]);
        snprintf(l7, sizeof(l7), "Battery: %s", power_names[s_cfg.power]);
        const char *items[] = { l0, l1, l2, l3, l4, l5, l6, l7, "Wallpaper", "Preview", "Back" };
        int idx = k85_run_list_menu("LOCK SCREEN", items, 11, nullptr);
        if (idx < 0 || idx == 10) return;
        switch (idx) {
            case 0: choose_scene(); break;
            case 1: s_cfg.shape_ovr = (s_cfg.shape_ovr + 2) % 9 - 1; cfg_save(); break;
            case 2: g_config.lock_particle_color = COLOR_PRESETS[(ci + 1) % COLOR_PRESET_COUNT]; k85_config_save(); break;
            case 3: s_cfg.speed = (s_cfg.speed + 1) % 3; cfg_save(); break;
            case 4: s_cfg.density = (s_cfg.density + 1) % 3; cfg_save(); break;
            case 5: s_cfg.expl = (s_cfg.expl + 2) % 5 - 1; cfg_save(); break;
            case 6: s_cfg.estyle = (s_cfg.estyle + 2) % 4 - 1; cfg_save(); break;
            case 7: s_cfg.power = (s_cfg.power + 1) % 3; cfg_save(); break;
            case 8: k85_look_wall_menu(K85_WALL_LOCK); break;
            case 9: preview(); break;
            default: break;
        }
    }
}

// K85FX2: battery. How often a frame is drawn depends on how long nobody touched the stick.
int k85_lockfx_power(void) { cfg_load(); return s_cfg.power; }
bool k85_lockfx_busy(void) { return s_nb > 0 || s_ring_on; }
uint32_t k85_lockfx_interval(uint32_t idle_ms) {
    cfg_load();
    s_frozen = false;
    switch (s_cfg.power) {
        case 0:   // Saver: the picture freezes after 40 s, only the clock keeps ticking
            if (idle_ms < 8000) return 66;
            if (idle_ms < 40000) return 200;
            s_frozen = true;
            return 1000;
        case 2:   // Smooth
            if (idle_ms < 90000) return 33;
            return 100;
        default:  // Balanced
            if (idle_ms < 20000) return 40;
            if (idle_ms < 90000) return 100;
            return 250;
    }
}