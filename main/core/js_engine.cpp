// K85LUA1: Lua 5.4 (MIT) scripts. Shell: lua <file> | lua -e "code". Menu: Lua. KStore runs .lua too.
// ui=true: screen + buttons (menu), ui=false: headless (shell / cron / startup.sh, 20 s limit).
#include "js_engine.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "esp_attr.h"
#include "config.h"
#include "theme.h"
#include "input.h"
#include "sound.h"
#include "battery.h"
#include "rtc_ntp.h"
#include "wifi.h"
#include "fs_guard.h"
#include "shell_commands.h"
#include "common.h"
#include "list_menu.h"
#include "text_input.h"

#include "M5Unified.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#define JN_DIR          "/littlefs/scripts"
#define JN_HEADLESS_US  20000000LL
#define JN_FILE_MAX     (64 * 1024)
#define JN_MEM_MAX      (1024 * 1024)

struct JnCtx {
    bool ui, abort;
    char *out;
    size_t cap, len;
    M5Canvas *cv;
    int down, hit;
    int64_t t0;
};
static JnCtx s_c;
static bool s_running = false;
static size_t s_mem = 0;

bool k85_js_running(void) { return s_running; }

static void jn_out(const char *s) {
    if (!s_c.out || !s_c.cap || !s) return;
    size_t n = strlen(s);
    if (s_c.len + n + 1 > s_c.cap) n = (s_c.cap > s_c.len + 1) ? s_c.cap - s_c.len - 1 : 0;
    memcpy(s_c.out + s_c.len, s, n);
    s_c.len += n;
    s_c.out[s_c.len] = 0;
}

// Lua memory comes from PSRAM (internal RAM is too small), capped at JN_MEM_MAX
static void *jn_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (!ptr) osize = 0;
    if (nsize == 0) {
        if (ptr) heap_caps_free(ptr);
        s_mem -= osize;
        return nullptr;
    }
    if (s_mem - osize + nsize > JN_MEM_MAX) return nullptr;
    void *p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) s_mem = s_mem - osize + nsize;
    return p;
}

// true = the script has to stop (A+B held in the menu, or the 20 s limit when headless)
static bool jn_poll(void) {
    if (!s_c.ui) {
        if (esp_timer_get_time() - s_c.t0 > JN_HEADLESS_US) s_c.abort = true;
        return s_c.abort;
    }
    k85_input_update();
    if (k85_btn_a_pressed()) s_c.hit |= 1;
    if (k85_btn_b_pressed()) s_c.hit |= 2;
    s_c.down = (k85_btn_a_is_down() ? 1 : 0) | (k85_btn_b_is_down() ? 2 : 0);
    if (k85_ab_held(500)) s_c.abort = true;
    return s_c.abort;
}

static void jn_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    if (jn_poll()) luaL_error(L, "aborted");
}

static bool jn_path(const char *in, char *out, size_t n) {
    const char *p = in;
    size_t rl = strlen("/littlefs");
    if (!strncmp(p, "/littlefs", rl) && (p[rl] == '/' || p[rl] == 0)) p += rl;
    while (*p == '/') p++;
    if (!*p || strstr(p, "..")) return false;
    snprintf(out, n, "/littlefs/%s", p);
    return !k85_is_blocked_path(out);
}

#define JN_ABORT()  do { if (jn_poll()) return luaL_error(L, "aborted"); } while (0)
#define JN_GFX()    do { if (!s_c.ui || !s_c.cv) return luaL_error(L, "no display"); } while (0)

static int ai(lua_State *L, int i) { return (int)luaL_checknumber(L, i); }
static uint32_t ac(lua_State *L, int i, uint32_t def) { return (uint32_t)((long long)luaL_optnumber(L, i, (double)def)) & 0xFFFFFF; }

// ---------- general ----------
static int l_print(lua_State *L) {
    JN_ABORT();
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        const char *s = luaL_tolstring(L, i, nullptr);
        if (i > 1) jn_out(" ");
        jn_out(s);
        lua_pop(L, 1);
    }
    jn_out("\r\n");
    return 0;
}
static int l_delay(lua_State *L) {
    int ms = (int)luaL_optnumber(L, 1, 0);
    if (ms < 0) ms = 0;
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (true) {
        JN_ABORT();
        if (esp_timer_get_time() >= end) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return 0;
}
static int l_millis(lua_State *L) { lua_pushinteger(L, (long long)(esp_timer_get_time() / 1000)); return 1; }
static int l_rand(lua_State *L) {
    long long m = (long long)luaL_optnumber(L, 1, 1);
    if (m < 1) m = 1;
    lua_pushinteger(L, (long long)(esp_random() % (uint32_t)m));
    return 1;
}
static int l_rgb(lua_State *L) {
    int r = ai(L, 1) & 255, g = ai(L, 2) & 255, b = ai(L, 3) & 255;
    lua_pushinteger(L, (long long)((r << 16) | (g << 8) | b));
    return 1;
}

// ---------- files / system ----------
static int l_read(lua_State *L) {
    JN_ABORT();
    const char *p = luaL_checkstring(L, 1);
    char full[160];
    if (!jn_path(p, full, sizeof(full))) return luaL_error(L, "bad path");
    FILE *f = fopen(full, "rb");
    if (!f) { lua_pushnil(L); return 1; }
    static EXT_RAM_BSS_ATTR char buf[4096];
    size_t r = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    lua_pushlstring(L, buf, r);
    return 1;
}
static int jn_wr(lua_State *L, const char *mode) {
    JN_ABORT();
    const char *p = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *s = luaL_checklstring(L, 2, &len);
    char full[160];
    if (!jn_path(p, full, sizeof(full))) return luaL_error(L, "bad path");
    FILE *f = fopen(full, mode);
    if (!f) { lua_pushnil(L); return 1; }
    size_t w = fwrite(s, 1, len, f);
    fclose(f);
    lua_pushinteger(L, (long long)w);
    return 1;
}
static int l_write(lua_State *L) { return jn_wr(L, "wb"); }
static int l_append(lua_State *L) { return jn_wr(L, "ab"); }
static int l_exists(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    char full[160];
    struct stat st;
    lua_pushboolean(L, jn_path(p, full, sizeof(full)) && stat(full, &st) == 0);
    return 1;
}
static int l_sh(lua_State *L) {
    JN_ABORT();
    const char *cmd = luaL_checkstring(L, 1);
    static EXT_RAM_BSS_ATTR char res[1024];
    res[0] = 0;
    k85_shell_run_command(cmd, res, sizeof(res));
    lua_pushstring(L, res);
    return 1;
}
static int l_tone(lua_State *L) {
    if (s_c.ui) k85_play_tone((uint32_t)luaL_checknumber(L, 1), (uint32_t)luaL_optnumber(L, 2, 100));
    return 0;
}
static int l_battery(lua_State *L) { lua_pushinteger(L, k85_get_battery()); return 1; }
static int l_ip(lua_State *L) { lua_pushstring(L, k85_wifi_get_ip_str()); return 1; }
static int l_time(lua_State *L) { lua_pushstring(L, k85_get_time_str()); return 1; }

// ---------- graphics (menu mode only) ----------
static int l_width(lua_State *L) { JN_GFX(); lua_pushinteger(L, s_c.cv->width()); return 1; }
static int l_height(lua_State *L) { JN_GFX(); lua_pushinteger(L, s_c.cv->height()); return 1; }
static int l_cls(lua_State *L) { JN_GFX(); s_c.cv->fillScreen(ac(L, 1, 0)); return 0; }
static int l_pixel(lua_State *L) { JN_GFX(); s_c.cv->drawPixel(ai(L, 1), ai(L, 2), ac(L, 3, 0xFFFFFF)); return 0; }
static int l_line(lua_State *L) { JN_GFX(); s_c.cv->drawLine(ai(L, 1), ai(L, 2), ai(L, 3), ai(L, 4), ac(L, 5, 0xFFFFFF)); return 0; }
static int l_rect(lua_State *L) { JN_GFX(); s_c.cv->drawRect(ai(L, 1), ai(L, 2), ai(L, 3), ai(L, 4), ac(L, 5, 0xFFFFFF)); return 0; }
static int l_frect(lua_State *L) { JN_GFX(); s_c.cv->fillRect(ai(L, 1), ai(L, 2), ai(L, 3), ai(L, 4), ac(L, 5, 0xFFFFFF)); return 0; }
static int l_circle(lua_State *L) { JN_GFX(); s_c.cv->drawCircle(ai(L, 1), ai(L, 2), ai(L, 3), ac(L, 4, 0xFFFFFF)); return 0; }
static int l_fcircle(lua_State *L) { JN_GFX(); s_c.cv->fillCircle(ai(L, 1), ai(L, 2), ai(L, 3), ac(L, 4, 0xFFFFFF)); return 0; }
static int l_text(lua_State *L) {
    JN_GFX();
    const char *s = luaL_checkstring(L, 3);
    int sz = (int)luaL_optnumber(L, 5, 1);
    if (sz < 1) sz = 1;
    if (sz > 6) sz = 6;
    s_c.cv->setTextSize(sz);
    s_c.cv->setTextColor(ac(L, 4, 0xFFFFFF));
    s_c.cv->setCursor(ai(L, 1), ai(L, 2));
    s_c.cv->print(s);
    return 0;
}
static int l_flip(lua_State *L) {
    JN_GFX();
    s_c.cv->pushSprite(0, 0);
    JN_ABORT();
    return 0;
}
static int l_btn(lua_State *L) { JN_ABORT(); lua_pushinteger(L, s_c.down); return 1; }                       // 1 = A held, 2 = B held
static int l_hit(lua_State *L) { JN_ABORT(); lua_pushinteger(L, s_c.hit); s_c.hit = 0; return 1; }           // pressed since the last call

static const luaL_Reg k_fn[] = {
    {"print", l_print}, {"delay", l_delay}, {"millis", l_millis}, {"rand", l_rand}, {"rgb", l_rgb},
    {"read", l_read}, {"write", l_write}, {"append", l_append}, {"exists", l_exists}, {"sh", l_sh},
    {"tone", l_tone}, {"battery", l_battery}, {"ip", l_ip}, {"time", l_time},
    {"width", l_width}, {"height", l_height}, {"cls", l_cls}, {"pixel", l_pixel}, {"line", l_line},
    {"rect", l_rect}, {"frect", l_frect}, {"circle", l_circle}, {"fcircle", l_fcircle}, {"text", l_text},
    {"flip", l_flip}, {"btn", l_btn}, {"hit", l_hit},
    {nullptr, nullptr}
};
struct JnConst { const char *n; long v; };
static const JnConst k_const[] = {
    {"BLACK", 0x000000}, {"WHITE", 0xFFFFFF}, {"RED", 0xFF0000}, {"GREEN", 0x00FF00}, {"BLUE", 0x0000FF},
    {"YELLOW", 0xFFFF00}, {"CYAN", 0x00FFFF}, {"MAGENTA", 0xFF00FF}, {"GRAY", 0x808080}, {"ORANGE", 0xFF8000},
};

static void jn_open(lua_State *L) {
    luaL_requiref(L, "_G", luaopen_base, 1);                 lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);     lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);      lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);      lua_pop(L, 1);
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    luaL_dostring(L, "local l=load;load=function(c,n,m,...) return l(c,n,'t',...) end;string.dump=nil");  // K85SEC9: text chunks only
    lua_pushglobaltable(L);
    luaL_setfuncs(L, k_fn, 0);
    lua_pop(L, 1);
    for (size_t i = 0; i < sizeof(k_const) / sizeof(k_const[0]); i++) {
        lua_pushinteger(L, (long long)k_const[i].v);
        lua_setglobal(L, k_const[i].n);
    }
}

static bool jn_run(const char *code, size_t len, const char *name, char *out, size_t out_size, bool ui, bool show_result) {
    if (s_running) {
        if (out && out_size) snprintf(out, out_size, "lua: already running\r\n");
        return false;
    }
    s_running = true;
    memset(&s_c, 0, sizeof(s_c));
    s_c.ui = ui;
    s_c.out = out;
    s_c.cap = out_size;
    s_c.t0 = esp_timer_get_time();
    s_mem = 0;
    if (out && out_size) out[0] = 0;

    lua_State *L = lua_newstate(jn_alloc, nullptr);
    if (!L) {
        jn_out("lua: out of memory\r\n");
        s_running = false;
        return false;
    }
    jn_open(L);

    if (ui) {
        M5Canvas *cv = new M5Canvas(&M5.Display);
        cv->setPsram(true);
        if (cv->createSprite(M5.Display.width(), M5.Display.height())) {
            cv->fillScreen(0x000000);
            s_c.cv = cv;
        } else {
            delete cv;
        }
    }
    lua_sethook(L, jn_hook, LUA_MASKCOUNT, 2000);

    bool ok = true;
    int top = lua_gettop(L);
    int rc = LUA_ERRSYNTAX;
    if (show_result) {
        static EXT_RAM_BSS_ATTR char ret[640];
        int n = snprintf(ret, sizeof(ret), "return %s", code);
        if (n > 0 && n < (int)sizeof(ret)) rc = luaL_loadbuffer(L, ret, (size_t)n, name);
        if (rc != LUA_OK) { lua_settop(L, top); rc = luaL_loadbuffer(L, code, len, name); }
    } else {
        rc = luaL_loadbuffer(L, code, len, name);
    }
    if (rc == LUA_OK) rc = lua_pcall(L, 0, show_result ? LUA_MULTRET : 0, 0);

    if (rc != LUA_OK) {
        if (s_c.abort) {
            jn_out("(stopped)\r\n");
        } else {
            const char *m = lua_tostring(L, -1);
            jn_out("error: ");
            jn_out(m ? m : "?");
            jn_out("\r\n");
        }
        ok = false;
    } else if (show_result) {
        int nres = lua_gettop(L) - top;
        for (int i = 1; i <= nres; i++) {
            int t = lua_type(L, top + i);
            const char *s = (t == LUA_TSTRING || t == LUA_TNUMBER) ? lua_tostring(L, top + i) : luaL_typename(L, top + i);
            jn_out(s);
            jn_out(i < nres ? " " : "\r\n");
        }
    }

    lua_close(L);
    if (s_c.cv) { s_c.cv->deleteSprite(); delete s_c.cv; s_c.cv = nullptr; }
    if (ui) {
        M5.Display.fillScreen(k85_get_bg());
        k85_wait_ab_release();
    }
    s_running = false;
    return ok;
}

bool k85_js_run_code(const char *code, char *out, size_t out_size, bool ui) {
    return jn_run(code, strlen(code), "=code", out, out_size, ui, true);
}

bool k85_js_run_file(const char *path, char *out, size_t out_size, bool ui) {
    char p[160];
    snprintf(p, sizeof(p), "%s", path ? path : "");
    size_t pl = strlen(p);
    while (pl && (p[pl - 1] == ' ' || p[pl - 1] == '\r' || p[pl - 1] == '\n')) p[--pl] = 0;
    char full[200];
    if (!jn_path(p, full, sizeof(full))) { if (out && out_size) snprintf(out, out_size, "lua: bad path\r\n"); return false; }
    struct stat st;
    if (stat(full, &st) != 0) { if (out && out_size) snprintf(out, out_size, "lua: cannot open %.60s\r\n", full); return false; }
    if (st.st_size > JN_FILE_MAX) { if (out && out_size) snprintf(out, out_size, "lua: file too large (max 64 KB)\r\n"); return false; }
    size_t sz = (size_t)st.st_size;
    char *buf = (char *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { if (out && out_size) snprintf(out, out_size, "lua: out of memory\r\n"); return false; }
    FILE *f = fopen(full, "rb");
    if (!f) { free(buf); if (out && out_size) snprintf(out, out_size, "lua: cannot open\r\n"); return false; }
    size_t r = fread(buf, 1, sz, f);
    fclose(f);
    buf[r] = 0;
    const char *bn = strrchr(full, '/');
    char cname[64];
    snprintf(cname, sizeof(cname), "@%s", bn ? bn + 1 : full);
    bool ok = jn_run(buf, r, cname, out, out_size, ui, false);
    free(buf);
    return ok;
}

// ---------- menu ----------
static const char *K_HELLO = R"LUA(-- k85OS Lua 5.4: print, string, math, table + the k85 functions
print("Hello from Lua")
for i = 1, 3 do print("tick", i) end
print("battery " .. battery() .. "%  ip " .. ip())
print(string.format("pi = %.3f", math.pi))
)LUA";

static const char *K_BOUNCE = R"LUA(-- ball demo: hold A+B to exit.  btn() = buttons held (1=A 2=B), hit() = pressed since last call
local x, y, dx, dy = 20, 20, 3, 2
local w, h = width(), height()
while true do
  cls(BLACK)
  fcircle(x, y, 6, YELLOW)
  text(2, 2, "hold A+B = exit", WHITE, 1)
  x = x + dx
  y = y + dy
  if x < 6 or x > w - 6 then dx = -dx end
  if y < 6 or y > h - 6 then dy = -dy end
  flip()
  delay(20)
end
)LUA";

static void jn_save(const char *name, const char *txt) {
    char p[96];
    snprintf(p, sizeof(p), "%s/%s", JN_DIR, name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite(txt, 1, strlen(txt), f);
    fclose(f);
}

static void jn_show(const char *out, bool ok) {
    char m[260];
    size_t k = 0;
    for (size_t i = 0; out[i] && k < 190; i++) if (out[i] != '\r') m[k++] = out[i];
    m[k] = 0;
    if (!k) snprintf(m, sizeof(m), "%s", ok ? "(done)" : "(failed)");
    strncat(m, "\nA+B=back", sizeof(m) - strlen(m) - 1);
    k85_show_message(m);
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_run_js_menu(void) {
    struct stat st;
    if (stat(JN_DIR, &st) != 0) mkdir(JN_DIR, 0755);
    char hp[96];
    snprintf(hp, sizeof(hp), "%s/hello.lua", JN_DIR);
    if (stat(hp, &st) != 0) {
        jn_save("hello.lua", K_HELLO);
        jn_save("bounce.lua", K_BOUNCE);
    }
    static EXT_RAM_BSS_ATTR char names[16][40];
    static EXT_RAM_BSS_ATTR char out[1536];
    static EXT_RAM_BSS_ATTR char code[200];
    while (true) {
        int count = 0;
        DIR *d = opendir(JN_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != nullptr && count < 16) {
                size_t l = strlen(e->d_name);
                if (l > 4 && l < sizeof(names[0]) && !strcmp(e->d_name + l - 4, ".lua")) {
                    snprintf(names[count], sizeof(names[0]), "%s", e->d_name);
                    count++;
                }
            }
            closedir(d);
        }
        const char *items[18];
        for (int i = 0; i < count; i++) items[i] = names[i];
        items[count] = "Run code line";
        items[count + 1] = "Back";
        int sel = k85_run_list_menu("LUA", items, count + 2, nullptr);
        if (sel < 0 || sel >= count + 1) return;
        if (sel == count) {
            code[0] = 0;
            if (!k85_text_input("Lua code:", "", code, sizeof(code))) continue;
            bool ok = jn_run(code, strlen(code), "=code", out, sizeof(out), true, true);
            jn_show(out, ok);
        } else {
            char path[96];
            snprintf(path, sizeof(path), "%s/%s", JN_DIR, names[sel]);
            bool ok = k85_js_run_file(path, out, sizeof(out), true);
            jn_show(out, ok);
        }
    }
}