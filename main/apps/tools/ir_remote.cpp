// K85IR2: IR remote v2 - IR TX / IR RX / Saved codes, portrait view while inside this tool
#include "ir_remote.h"
#include "common.h"
#include "theme.h"
#include "battery.h"
#include "status_bar.h"
#include "input.h"
#include "text_input.h"
#include "esp_log.h"
#include "list_menu.h"
#include "sound.h"

#include "M5Unified.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#define IR_SEND_PIN 46
#define IR_RECEIVE_PIN 42
#define IR_CARRIER_FREQ_HZ 38000
#define IR_DUTY_CYCLE 0.33f

#define NEC_HEADER_MARK 9000
#define NEC_HEADER_SPACE 4500
#define SAM_HEADER_MARK 4500
#define SAM_HEADER_SPACE 4500
#define NEC_BIT_MARK 560
#define NEC_BIT_0_SPACE 560
#define NEC_BIT_1_SPACE 1690

#define SIRC_HEADER_MARK 2400
#define SIRC_HEADER_SPACE 600
#define SIRC_BIT_SPACE 600
#define SIRC_BIT_0_MARK 600
#define SIRC_BIT_1_MARK 1200

#define RC5_HALF_BIT 889

#define K85_IR_MAX_CODES 24
#define K85_IR_RAW_MAX 128        // longest raw signal we store (pulses)
#define K85_IR_RX_MAX 192         // receive buffer (pulses)
#define K85_IR_ROT_STEP 3         // 3 = turn the view 90 deg left, 1 = 90 deg right
#define K85_IR_FILE_NAME "k85_ir2.txt"
#define K85_IR_OLD_FILE "/flash/k85_ir_codes.txt"

// order matters: index is used in K85_PROTO_CH
enum K85IrProtocol { IR_PROTO_NEC, IR_PROTO_SAMSUNG32, IR_PROTO_SIRC, IR_PROTO_NECEXT, IR_PROTO_RC5, IR_PROTO_SAMRAW };
static const char K85_PROTO_CH[] = "NSYXCW";

static const char *proto_short(K85IrProtocol p) {
    switch (p) {
        case IR_PROTO_NEC: return "NEC";
        case IR_PROTO_SAMSUNG32: return "SAM";
        case IR_PROTO_SIRC: return "SONY";
        case IR_PROTO_NECEXT: return "NECX";
        case IR_PROTO_RC5: return "RC5";
        case IR_PROTO_SAMRAW: return "SAMX";
    }
    return "?";
}

// ---------- built-in TV database (best effort, grouped by brand) ----------
struct K85IrBtn { const char *name; uint16_t command; };
struct K85IrBrand { const char *name; K85IrProtocol proto; uint16_t address; const K85IrBtn *btns; int n; };

static const K85IrBtn BTN_SAMSUNG[] = { {"Power", 0x02}, {"Vol +", 0x07}, {"Vol -", 0x0B}, {"Mute", 0x0F}, {"Ch +", 0x12}, {"Ch -", 0x10}, {"Source", 0x01} };
static const K85IrBtn BTN_SAMSUNG_OLD[] = { {"Power", 0x0C} };
static const K85IrBtn BTN_LG[] = { {"Power", 0x08}, {"Vol +", 0x02}, {"Vol -", 0x03}, {"Mute", 0x09}, {"Ch +", 0x00}, {"Ch -", 0x01}, {"Input", 0x0B} };
static const K85IrBtn BTN_SONY[] = { {"Power", 0x15}, {"Power alt", 0x2F}, {"Vol +", 0x12}, {"Vol -", 0x13}, {"Mute", 0x14}, {"Ch +", 0x10}, {"Ch -", 0x11}, {"Input", 0x25} };
static const K85IrBtn BTN_SONY_ALT[] = { {"Power", 0x15} };
static const K85IrBtn BTN_PHILIPS[] = { {"Power", 0x0C}, {"Vol +", 0x10}, {"Vol -", 0x11}, {"Mute", 0x0D}, {"Ch +", 0x20}, {"Ch -", 0x21} };
static const K85IrBtn BTN_NEC8[] = { {"Power A", 0x05}, {"Power B", 0x17} };
static const K85IrBtn BTN_EXT_A[] = { {"Power", 0x001C} };
static const K85IrBtn BTN_EXT_B[] = { {"Power", 0x3FC0} };

#define BR(nm, pr, ad, arr) { nm, pr, ad, arr, (int)(sizeof(arr) / sizeof(arr[0])) }
static const K85IrBrand K85_BRANDS[] = {
    BR("Samsung",       IR_PROTO_SAMSUNG32, 0x0007, BTN_SAMSUNG),
    BR("Samsung old",   IR_PROTO_SAMSUNG32, 0x000E, BTN_SAMSUNG_OLD),
    BR("LG / Haier",    IR_PROTO_NEC,       0x0004, BTN_LG),
    BR("Sony",          IR_PROTO_SIRC,      0x0001, BTN_SONY),
    BR("Sony alt",      IR_PROTO_SIRC,      0x0010, BTN_SONY_ALT),
    BR("Philips",       IR_PROTO_RC5,       0x0000, BTN_PHILIPS),
    BR("Generic NEC",   IR_PROTO_NEC,       0x0008, BTN_NEC8),
    BR("Generic EXT A", IR_PROTO_NECEXT,    0xDF00, BTN_EXT_A),
    BR("Generic EXT B", IR_PROTO_NECEXT,    0x1818, BTN_EXT_B),
};
#define K85_BRAND_COUNT (int)(sizeof(K85_BRANDS) / sizeof(K85_BRANDS[0]))

// ---------- saved code ----------
struct K85IrSaved {
    char name[16];
    uint8_t kind;      // 0 = decoded, 1 = raw
    uint8_t proto;     // K85IrProtocol (decoded)
    uint16_t address;
    uint16_t command;
    uint16_t raw_n;    // raw: number of pulses
    uint16_t raw[2 * K85_IR_RAW_MAX];  // raw: mark, space, ... in 10 us units (last space = 0)
};

static K85IrSaved *saved_alloc(void) {
    return (K85IrSaved *)heap_caps_calloc(K85_IR_MAX_CODES + 1, sizeof(K85IrSaved), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// ---------- RMT ----------
static rmt_channel_handle_t s_tx_chan = nullptr;
static rmt_channel_handle_t s_rx_chan = nullptr;
static rmt_encoder_handle_t s_copy_encoder = nullptr;
static bool s_rmt_inited = false;

static rmt_symbol_word_t s_rx_symbols[K85_IR_RX_MAX];
static rmt_symbol_word_t s_prev_symbols[K85_IR_RX_MAX];
static volatile bool s_rx_done = false;
static size_t s_rx_symbol_num = 0;

static bool rmt_rx_done_cb(rmt_channel_handle_t, const rmt_rx_done_event_data_t *edata, void *) {
    s_rx_symbol_num = edata->num_symbols;
    s_rx_done = true;
    return true;
}

static void start_rmt_receive(void) {
    rmt_receive_config_t cfg = {};
    cfg.signal_range_min_ns = 3000;      // hardware glitch filter (max ~3.1 us)
    cfg.signal_range_max_ns = 20000000;
    rmt_receive(s_rx_chan, s_rx_symbols, sizeof(s_rx_symbols), &cfg);
}

static bool ir_rmt_init(void) {
    if (s_rmt_inited) return true;

    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = (gpio_num_t)IR_SEND_PIN;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = 1000000;
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 4;
    if (rmt_new_tx_channel(&tx_cfg, &s_tx_chan) != ESP_OK) return false;

    rmt_carrier_config_t carrier_cfg = {};
    carrier_cfg.frequency_hz = IR_CARRIER_FREQ_HZ;
    carrier_cfg.duty_cycle = IR_DUTY_CYCLE;
    rmt_apply_carrier(s_tx_chan, &carrier_cfg);

    rmt_copy_encoder_config_t enc_cfg = {};
    rmt_new_copy_encoder(&enc_cfg, &s_copy_encoder);
    rmt_enable(s_tx_chan);

    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.gpio_num = (gpio_num_t)IR_RECEIVE_PIN;
    rx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz = 1000000;
    rx_cfg.mem_block_symbols = 128;
    if (rmt_new_rx_channel(&rx_cfg, &s_rx_chan) != ESP_OK) return false;

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rmt_rx_done_cb;
    rmt_rx_register_event_callbacks(s_rx_chan, &cbs, nullptr);
    rmt_enable(s_rx_chan);

    M5.Power.setExtOutput(true, m5::ext_none);

    s_rmt_inited = true;
    return true;
}

// ---------- encoders ----------
static uint32_t nec_raw(uint16_t address, uint8_t command) {
    uint16_t nec_addr;
    if (address <= 0x00FF) {
        uint8_t addr8 = address & 0xFF;
        nec_addr = ((uint16_t)(~addr8) << 8) | addr8;
    } else {
        nec_addr = address;
    }
    uint32_t raw = 0;
    raw |= (uint32_t)nec_addr;
    raw |= (uint32_t)command << 16;
    raw |= (uint32_t)(~command) << 24;
    return raw;
}

// Samsung32: address byte sent twice, then command and its inverse
static uint32_t samsung_raw(uint16_t address, uint16_t command) {
    uint32_t a = address & 0xFF, c = command & 0xFF;
    return a | (a << 8) | (c << 16) | ((~c & 0xFF) << 24);
}

// 32-bit frame, LSB first; NEC header 9000/4500, Samsung header 4500/4500
static void encode_nec_family(uint32_t raw_data, uint32_t hmark, uint32_t hspace, rmt_symbol_word_t *symbols, size_t *symbol_count) {
    size_t idx = 0;
    symbols[idx].duration0 = hmark; symbols[idx].level0 = 1;
    symbols[idx].duration1 = hspace; symbols[idx].level1 = 0;
    idx++;
    for (int i = 0; i < 32; i++) {
        symbols[idx].duration0 = NEC_BIT_MARK; symbols[idx].level0 = 1;
        symbols[idx].duration1 = (raw_data & (1UL << i)) ? NEC_BIT_1_SPACE : NEC_BIT_0_SPACE;
        symbols[idx].level1 = 0;
        idx++;
    }
    symbols[idx].duration0 = NEC_BIT_MARK; symbols[idx].level0 = 1;
    symbols[idx].duration1 = 0; symbols[idx].level1 = 0;
    idx++;
    *symbol_count = idx;
}

// Sony SIRC 12 bit: 7 bit command + 5 bit address, LSB first
static void encode_sirc(uint16_t address, uint16_t command, rmt_symbol_word_t *symbols, size_t *symbol_count) {
    uint16_t data = ((address & 0x1F) << 7) | (command & 0x7F);
    size_t idx = 0;
    symbols[idx].duration0 = SIRC_HEADER_MARK; symbols[idx].level0 = 1;
    symbols[idx].duration1 = SIRC_HEADER_SPACE; symbols[idx].level1 = 0;
    idx++;
    for (int i = 0; i < 12; i++) {
        bool bit1 = (data & (1U << i)) != 0;
        symbols[idx].duration0 = bit1 ? SIRC_BIT_1_MARK : SIRC_BIT_0_MARK;
        symbols[idx].level0 = 1;
        symbols[idx].duration1 = SIRC_BIT_SPACE;
        symbols[idx].level1 = 0;
        idx++;
    }
    *symbol_count = idx;
}

// Philips RC5: Manchester, 2 start bits + toggle + 5 bit address + 6 bit command
static void encode_rc5(uint16_t address, uint16_t command, rmt_symbol_word_t *symbols, size_t *symbol_count) {
    bool bits[14];
    bits[0] = true;
    bits[1] = true;
    bits[2] = false;
    for (int i = 0; i < 5; i++) bits[3 + i] = (address & (1 << (4 - i))) != 0;
    for (int i = 0; i < 6; i++) bits[8 + i] = (command & (1 << (5 - i))) != 0;
    size_t idx = 0;
    for (int i = 0; i < 14; i++) {
        if (bits[i]) {
            symbols[idx].duration0 = RC5_HALF_BIT; symbols[idx].level0 = 0;
            symbols[idx].duration1 = RC5_HALF_BIT; symbols[idx].level1 = 1;
        } else {
            symbols[idx].duration0 = RC5_HALF_BIT; symbols[idx].level0 = 1;
            symbols[idx].duration1 = RC5_HALF_BIT; symbols[idx].level1 = 0;
        }
        idx++;
    }
    *symbol_count = idx;
}

static bool transmit_symbols(rmt_symbol_word_t *symbols, size_t symbol_count) {
    rmt_transmit_config_t tx_cfg = {};
    tx_cfg.loop_count = 0;
    esp_err_t ret = rmt_transmit(s_tx_chan, s_copy_encoder, symbols,
                                 symbol_count * sizeof(rmt_symbol_word_t), &tx_cfg);
    if (ret == ESP_OK) ret = rmt_tx_wait_all_done(s_tx_chan, 1000);
    return ret == ESP_OK;
}

static bool send_ir_code(K85IrProtocol proto, uint16_t address, uint16_t command) {
    rmt_symbol_word_t sym[40];
    size_t n = 0;
    int repeats = 1;
    switch (proto) {
        case IR_PROTO_NEC:
            encode_nec_family(nec_raw(address, (uint8_t)command), NEC_HEADER_MARK, NEC_HEADER_SPACE, sym, &n); break;
        case IR_PROTO_NECEXT:
            encode_nec_family(((uint32_t)command << 16) | address, NEC_HEADER_MARK, NEC_HEADER_SPACE, sym, &n); break;
        case IR_PROTO_SAMSUNG32:
            encode_nec_family(samsung_raw(address, command), SAM_HEADER_MARK, SAM_HEADER_SPACE, sym, &n); break;
        case IR_PROTO_SAMRAW:
            encode_nec_family(((uint32_t)command << 16) | address, SAM_HEADER_MARK, SAM_HEADER_SPACE, sym, &n); break;
        case IR_PROTO_SIRC:
            encode_sirc(address, command, sym, &n); repeats = 3; break;   // TVs want Sony frames repeated
        case IR_PROTO_RC5:
            encode_rc5(address, command, sym, &n); break;
    }
    if (n == 0) return false;
    bool ok = true;
    for (int r = 0; r < repeats; r++) {
        if (!transmit_symbols(sym, n)) ok = false;
        if (r + 1 < repeats) vTaskDelay(pdMS_TO_TICKS(25));
    }
    return ok;
}

static bool send_raw(const uint16_t *v, uint16_t n) {
    if (n < 2 || n > K85_IR_RAW_MAX) return false;
    rmt_symbol_word_t sym[K85_IR_RAW_MAX];
    for (int i = 0; i < n; i++) {
        sym[i].level0 = 1;
        sym[i].duration0 = (uint32_t)v[2 * i] * 10;
        sym[i].level1 = 0;
        sym[i].duration1 = (uint32_t)v[2 * i + 1] * 10;
    }
    return transmit_symbols(sym, n);
}

static bool send_saved(const K85IrSaved *s) {
    if (s->kind == 1) return send_raw(s->raw, s->raw_n);
    return send_ir_code((K85IrProtocol)s->proto, s->address, s->command);
}

// ---------- compact UI helpers (portrait, small text) ----------
static int s_old_rot = 0;

static void ir_enter_screen(void) {
    s_old_rot = M5.Display.getRotation();
    int r = (s_old_rot & 4) | (((s_old_rot & 3) + K85_IR_ROT_STEP) & 3);
    M5.Display.setRotation(r);
    M5.Display.fillScreen(k85_get_bg());
}

static void ir_leave_screen(void) {
    M5.Display.setRotation(s_old_rot);   // always back to the normal orientation
    M5.Display.fillScreen(k85_get_bg());
}

static void ir_msg(const char *text) {
    uint32_t bg = k85_get_bg();
    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextWrap(true);
    M5.Display.setTextColor(0xFFFFFF, bg);
    M5.Display.setCursor(3, 16);
    M5.Display.print(text);
    k85_status_bar_overlay();
}

static void ir_toast(const char *text, int ms = 700) {
    ir_msg(text);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- storage: one text line per code, append-only for new codes ----------
//   D|name|P|addr|cmd          decoded (P = N S Y X C W), hex
//   R|name|n|v v v v ...        raw, n pulses, values in 10 us units
static const char *ir_base(void) {
    static char base[16] = "";
    if (base[0]) return base;
    static const char *cands[] = { "/littlefs", "/flash" };
    for (int i = 0; i < 2; i++) {
        char p[48];
        snprintf(p, sizeof(p), "%s/%s", cands[i], K85_IR_FILE_NAME);
        FILE *f = fopen(p, "a");
        if (f) { fclose(f); snprintf(base, sizeof(base), "%s", cands[i]); return base; }
    }
    snprintf(base, sizeof(base), "/littlefs");
    return base;
}

static void ir_path(char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", ir_base(), K85_IR_FILE_NAME);
}

static void name_clean(char *s) {
    for (; *s; s++) if (*s == '|' || (unsigned char)*s < 32) *s = '_';
}

static int split_bar(char *line, char *tok[], int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        tok[n++] = p;
        char *q = strchr(p, '|');
        if (!q) break;
        *q = 0;
        p = q + 1;
    }
    return n;
}

static bool parse_line(char *line, K85IrSaved *s) {
    char *t[5];
    int nt = split_bar(line, t, 5);
    memset(s, 0, sizeof(*s));
    if (nt >= 5 && t[0][0] == 'D' && t[2][0]) {
        const char *pc = strchr(K85_PROTO_CH, t[2][0]);
        if (!pc) return false;
        s->kind = 0;
        s->proto = (uint8_t)(pc - K85_PROTO_CH);
        s->address = (uint16_t)strtoul(t[3], nullptr, 16);
        s->command = (uint16_t)strtoul(t[4], nullptr, 16);
    } else if (nt >= 4 && t[0][0] == 'R') {
        unsigned n = (unsigned)strtoul(t[2], nullptr, 10);
        if (n < 2 || n > K85_IR_RAW_MAX) return false;
        char *p = t[3];
        for (unsigned i = 0; i < 2 * n; i++) {
            char *e = nullptr;
            unsigned long v = strtoul(p, &e, 10);
            if (e == p) return false;
            s->raw[i] = (uint16_t)v;
            p = e;
        }
        s->kind = 1;
        s->raw_n = (uint16_t)n;
    } else {
        return false;
    }
    snprintf(s->name, sizeof(s->name), "%.15s", t[1]);
    return s->name[0] != 0;
}

static void write_line(FILE *f, const K85IrSaved *s) {
    if (s->kind == 0) {
        fprintf(f, "D|%s|%c|%X|%X\n", s->name, K85_PROTO_CH[s->proto % 6], (unsigned)s->address, (unsigned)s->command);
    } else {
        fprintf(f, "R|%s|%u|", s->name, (unsigned)s->raw_n);
        for (unsigned i = 0; i < 2u * s->raw_n; i++) fprintf(f, i ? " %u" : "%u", (unsigned)s->raw[i]);
        fprintf(f, "\n");
    }
}

static bool ir_rewrite(const K85IrSaved *arr, int n) {
    char path[48], tmp[56];
    ir_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    for (int i = 0; i < n; i++) write_line(f, &arr[i]);
    fclose(f);
    if (rename(tmp, path) != 0) {
        remove(path);
        return rename(tmp, path) == 0;
    }
    return true;
}

// one small append per newly saved code: no rewriting of the whole file, so flash wear stays minimal
static bool ir_append(const K85IrSaved *s) {
    char path[48];
    ir_path(path, sizeof(path));
    FILE *f = fopen(path, "a");
    if (!f) return false;
    write_line(f, s);
    fclose(f);
    return true;
}

// codes saved by the old version of this tool (NEC only)
static int ir_import_old(K85IrSaved *out, int max_out) {
    FILE *f = fopen(K85_IR_OLD_FILE, "r");
    if (!f) return 0;
    char line[64];
    int n = 0;
    while (n < max_out && fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *p1 = strchr(line, '|'); if (!p1) continue;
        *p1 = 0;
        char *p2 = strchr(p1 + 1, '|'); if (!p2) continue;
        *p2 = 0;
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].name, sizeof(out[n].name), "%.15s", line);
        name_clean(out[n].name);
        out[n].kind = 0;
        out[n].proto = IR_PROTO_NEC;
        out[n].address = (uint16_t)atoi(p1 + 1);
        out[n].command = (uint16_t)atoi(p2 + 1);
        n++;
    }
    fclose(f);
    return n;
}

static int ir_load(K85IrSaved *out, int max_out) {
    char path[48];
    ir_path(path, sizeof(path));
    int n = 0;
    char *line = (char *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    FILE *f = line ? fopen(path, "r") : nullptr;
    if (f) {
        while (n < max_out && fgets(line, 2048, f)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            nl = strchr(line, '\r'); if (nl) *nl = 0;
            if (parse_line(line, &out[n])) n++;
        }
        fclose(f);
    }
    if (line) heap_caps_free(line);
    if (n == 0) {
        n = ir_import_old(out, max_out);
        if (n > 0) {
            ir_rewrite(out, n);
            rename(K85_IR_OLD_FILE, K85_IR_OLD_FILE ".done");
        }
    }
    return n;
}

static bool raw_close(const uint16_t *a, const uint16_t *b, int cnt) {
    for (int i = 0; i < cnt; i++) {
        int d = abs((int)a[i] - (int)b[i]);
        if (d > 3 + (int)a[i] / 5) return false;
    }
    return true;
}

static int find_same(const K85IrSaved *arr, int n, const K85IrSaved *s) {
    for (int i = 0; i < n; i++) {
        if (arr[i].kind != s->kind) continue;
        if (s->kind == 0) {
            if (arr[i].proto == s->proto && arr[i].address == s->address && arr[i].command == s->command) return i;
        } else if (arr[i].raw_n == s->raw_n && raw_close(arr[i].raw, s->raw, 2 * s->raw_n)) {
            return i;
        }
    }
    return -1;
}

// ---------- receive: noise filtering and decoding ----------
// a real remote signal has sane pulse lengths; short glitches and lone bursts are dropped
static bool ir_frame_sane(const rmt_symbol_word_t *s, size_t n) {
    if (n < 6) return false;
    for (size_t i = 0; i < n; i++) {
        uint32_t m = s[i].duration0, sp = s[i].duration1;
        if (m < 200 || m > 12000) return false;
        if (i + 1 < n && (sp < 200 || sp > 25000)) return false;
    }
    return true;
}

// two captures with (nearly) the same timings = a real button press, not noise
static bool ir_similar(const rmt_symbol_word_t *a, size_t na, const rmt_symbol_word_t *b, size_t nb) {
    if (na != nb || na < 6) return false;
    for (size_t i = 0; i < na; i++) {
        int32_t d0 = abs((int32_t)a[i].duration0 - (int32_t)b[i].duration0);
        if (d0 > 150 + (int32_t)a[i].duration0 / 5) return false;
        if (i + 1 < na) {
            int32_t d1 = abs((int32_t)a[i].duration1 - (int32_t)b[i].duration1);
            if (d1 > 150 + (int32_t)a[i].duration1 / 5) return false;
        }
    }
    return true;
}

// *strong = the frame validates itself (checksum), so a single capture is enough
static bool ir_decode_frame(const rmt_symbol_word_t *s, size_t n, K85IrSaved *o, bool *strong) {
    *strong = false;
    uint32_t hm = s[0].duration0, hs = s[0].duration1;
    bool nec = hm > 7500 && hm < 10500 && hs > 3500 && hs < 5500;
    bool sam = hm > 3500 && hm < 5500 && hs > 3500 && hs < 5500;
    if ((nec || sam) && n >= 34) {
        uint32_t raw = 0;
        for (int i = 0; i < 32; i++) {
            uint32_t mk = s[i + 1].duration0, sp = s[i + 1].duration1;
            if (mk < 250 || mk > 900) return false;
            if (sp >= 250 && sp <= 900) {
                // bit 0
            } else if (sp >= 1100 && sp <= 2600) {
                raw |= (1UL << i);
            } else {
                return false;
            }
        }
        memset(o, 0, sizeof(*o));
        o->kind = 0;
        o->proto = nec ? IR_PROTO_NECEXT : IR_PROTO_SAMRAW;
        o->address = (uint16_t)(raw & 0xFFFF);
        o->command = (uint16_t)(raw >> 16);
        *strong = ((uint8_t)((raw >> 16) ^ (raw >> 24)) == 0xFF);
        return true;
    }
    if (hm > 2000 && hm < 2900 && hs > 400 && hs < 900 && n >= 13) {
        uint16_t data = 0;
        for (int i = 0; i < 12; i++) {
            uint32_t mk = s[i + 1].duration0;
            if (i < 11 && (s[i + 1].duration1 < 400 || s[i + 1].duration1 > 900)) return false;
            if (mk >= 1000 && mk <= 1500) data |= (uint16_t)(1U << i);
            else if (mk < 400 || mk > 900) return false;
        }
        memset(o, 0, sizeof(*o));
        o->kind = 0;
        o->proto = IR_PROTO_SIRC;
        o->address = (data >> 7) & 0x1F;
        o->command = data & 0x7F;
        *strong = true;
        return true;
    }
    return false;
}

static void make_raw(const rmt_symbol_word_t *s, size_t n, K85IrSaved *o) {
    memset(o, 0, sizeof(*o));
    o->kind = 1;
    o->raw_n = (uint16_t)n;
    for (size_t i = 0; i < n; i++) {
        uint32_t m = (s[i].duration0 + 5) / 10;
        uint32_t sp = (i + 1 == n) ? 0 : (s[i].duration1 + 5) / 10;
        o->raw[2 * i] = (uint16_t)m;
        o->raw[2 * i + 1] = (uint16_t)sp;
    }
}

// canonical form (exact frames of NEC / Samsung are mapped back to their friendly protocol)
static void ir_canon(const K85IrSaved *c, K85IrProtocol *p, uint16_t *a, uint16_t *cm) {
    uint8_t b0 = c->address & 0xFF, b1 = (uint8_t)(c->address >> 8);
    uint8_t b2 = c->command & 0xFF, b3 = (uint8_t)(c->command >> 8);
    *p = (K85IrProtocol)c->proto;
    *a = c->address;
    *cm = c->command;
    if (c->proto == IR_PROTO_NECEXT && (uint8_t)(b0 ^ b1) == 0xFF && (uint8_t)(b2 ^ b3) == 0xFF) {
        *p = IR_PROTO_NEC; *a = b0; *cm = b2;
    } else if (c->proto == IR_PROTO_SAMRAW && b0 == b1 && (uint8_t)(b2 ^ b3) == 0xFF) {
        *p = IR_PROTO_SAMSUNG32; *a = b0; *cm = b2;
    }
}

static bool ir_find_known(const K85IrSaved *c, char *out, size_t cap) {
    if (c->kind != 0) return false;
    K85IrProtocol p;
    uint16_t a, cm;
    ir_canon(c, &p, &a, &cm);
    for (int i = 0; i < K85_BRAND_COUNT; i++) {
        const K85IrBrand &br = K85_BRANDS[i];
        if (br.proto != p || br.address != a) continue;
        for (int j = 0; j < br.n; j++) {
            if (br.btns[j].command == cm) { snprintf(out, cap, "%s %s", br.name, br.btns[j].name); return true; }
        }
    }
    return false;
}

static void ir_describe(const K85IrSaved *c, char *out, size_t cap) {
    if (c->kind == 1) { snprintf(out, cap, "RAW %u pulses", (unsigned)c->raw_n); return; }
    K85IrProtocol p;
    uint16_t a, cm;
    ir_canon(c, &p, &a, &cm);
    switch (p) {
        case IR_PROTO_NEC:       snprintf(out, cap, "NEC A:%02X C:%02X", (unsigned)a, (unsigned)cm); break;
        case IR_PROTO_SAMSUNG32: snprintf(out, cap, "Samsung %02X %02X", (unsigned)a, (unsigned)cm); break;
        case IR_PROTO_SIRC:      snprintf(out, cap, "Sony A:%02X C:%02X", (unsigned)a, (unsigned)cm); break;
        case IR_PROTO_NECEXT:    snprintf(out, cap, "NECext %04X %04X", (unsigned)a, (unsigned)cm); break;
        case IR_PROTO_SAMRAW:    snprintf(out, cap, "SamExt %04X %04X", (unsigned)a, (unsigned)cm); break;
        case IR_PROTO_RC5:       snprintf(out, cap, "RC5 A:%02X C:%02X", (unsigned)a, (unsigned)cm); break;
    }
}

static void ir_default_name(const K85IrSaved *c, char *out, size_t cap) {
    char known[32];
    if (ir_find_known(c, known, sizeof(known))) { snprintf(out, cap, "%.15s", known); return; }
    if (c->kind == 1) { snprintf(out, cap, "RAW %u", (unsigned)c->raw_n); return; }
    K85IrProtocol p;
    uint16_t a, cm;
    ir_canon(c, &p, &a, &cm);
    snprintf(out, cap, "%s %02X %02X", proto_short(p), (unsigned)(a & 0xFF), (unsigned)(cm & 0xFF));
}

// ---------- UI: IR RX ----------
static void rx_draw(uint32_t seen, uint32_t noise, const char *status) {
    uint32_t bg = k85_get_bg();
    M5.Display.fillScreen(bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextWrap(true);
    M5.Display.setTextColor(k85_get_accent(), bg);
    M5.Display.setCursor(3, 4);
    M5.Display.print("IR RX");
    M5.Display.setTextColor(0xFFFFFF, bg);
    M5.Display.setCursor(3, 22);
    M5.Display.print("Point the remote at\nthe stick and press\na button (hold it\nfor a second).");
    M5.Display.setCursor(3, 80);
    M5.Display.printf("Frames: %lu\nNoise ignored: %lu", (unsigned long)seen, (unsigned long)noise);
    M5.Display.setTextColor(0x07E0, bg);
    M5.Display.setCursor(3, 108);
    M5.Display.print(status);
    M5.Display.setTextColor(0xAAAAAA, bg);
    M5.Display.setCursor(3, M5.Display.height() - 12);
    M5.Display.print("A+B = back");
    k85_status_bar_overlay();
}

// returns true to keep listening, false to leave IR RX
static bool rx_handle_result(K85IrSaved *arr, int *count, const K85IrSaved *cand) {
    char title[28];
    ir_describe(cand, title, sizeof(title));
    static const char *acts[] = { "Save", "Test send", "Retry", "Back" };
    while (true) {
        int a = k85_run_list_menu(title, acts, 4, nullptr);
        if (a < 0 || a == 3) return false;
        if (a == 2) return true;
        if (a == 1) {
            bool ok = send_saved(cand);
            ir_toast(ok ? "Sent" : "Send failed", 500);
            continue;
        }
        int dup = find_same(arr, *count, cand);
        if (dup >= 0) {
            char m[64];
            snprintf(m, sizeof(m), "Already saved as:\n%s", arr[dup].name);
            ir_toast(m, 1500);
            continue;
        }
        if (*count >= K85_IR_MAX_CODES) { ir_toast("List is full.\nDelete some codes\nin Saved codes.", 1800); continue; }
        K85IrSaved s = *cand;
        char name[16];
        ir_default_name(cand, name, sizeof(name));
        if (!k85_text_input("Name:", name, name, sizeof(name))) continue;
        if (!name[0]) continue;
        name_clean(name);
        snprintf(s.name, sizeof(s.name), "%s", name);
        if (ir_append(&s)) {
            arr[*count] = s;
            (*count)++;
            ir_toast("Saved", 900);
        } else {
            ir_toast("Write failed", 1500);
        }
        return true;
    }
}

static void run_ir_rx(void) {
    K85IrSaved *arr = saved_alloc();
    if (!arr) { ir_msg("Out of memory\n\nA+B = back"); wait_ab_exit(); return; }
    K85IrSaved *cand = &arr[K85_IR_MAX_CODES];
    int count = ir_load(arr, K85_IR_MAX_CODES);

    uint32_t seen = 0, noise = 0;
    bool have_prev = false;
    size_t prev_n = 0;
    bool dirty = false;
    char status[48] = "Listening...";
    rx_draw(seen, noise, status);
    s_rx_done = false;
    start_rmt_receive();

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }

        if (s_rx_done) {
            s_rx_done = false;
            size_t n = s_rx_symbol_num;
            if (n > K85_IR_RX_MAX) n = K85_IR_RX_MAX;
            bool accepted = false;
            if (n >= 6) {
                seen++;
                if (!ir_frame_sane(s_rx_symbols, n)) {
                    noise++;
                } else {
                    bool similar = have_prev && ir_similar(s_prev_symbols, prev_n, s_rx_symbols, n);
                    bool strong = false;
                    if (ir_decode_frame(s_rx_symbols, n, cand, &strong)) {
                        accepted = strong || similar;
                        if (!accepted) snprintf(status, sizeof(status), "Weak signal,\npress again");
                    } else if (n > K85_IR_RAW_MAX) {
                        snprintf(status, sizeof(status), "Too long (%u pulses)", (unsigned)n);
                    } else if (similar) {
                        make_raw(s_rx_symbols, n, cand);
                        accepted = true;
                    } else {
                        snprintf(status, sizeof(status), "Unknown signal,\npress again");
                    }
                    memcpy(s_prev_symbols, s_rx_symbols, n * sizeof(rmt_symbol_word_t));
                    prev_n = n;
                    have_prev = true;
                }
                dirty = true;
            }
            if (accepted) {
                bool again = rx_handle_result(arr, &count, cand);
                if (!again) break;
                have_prev = false;
                snprintf(status, sizeof(status), "Listening...");
                dirty = true;
            }
            s_rx_done = false;
            start_rmt_receive();
        }
        if (dirty) { rx_draw(seen, noise, status); dirty = false; }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    heap_caps_free(arr);
}

// ---------- UI: Saved codes ----------
static void run_ir_saved(void) {
    K85IrSaved *arr = saved_alloc();
    if (!arr) { ir_msg("Out of memory\n\nA+B = back"); wait_ab_exit(); return; }
    while (true) {
        int n = ir_load(arr, K85_IR_MAX_CODES);
        if (n == 0) {
            ir_msg("No saved codes.\n\nUse IR RX to\ncapture some.\n\nA+B = back");
            wait_ab_exit();
            break;
        }
        const char *names[K85_IR_MAX_CODES + 1];
        for (int i = 0; i < n; i++) names[i] = arr[i].name;
        names[n] = "Back";
        int idx = k85_run_list_menu("SAVED CODES", names, n + 1, nullptr);
        if (idx < 0 || idx == n) break;

        static const char *acts[] = { "Send", "Rename", "Delete", "Back" };
        int act = k85_run_list_menu(arr[idx].name, acts, 4, nullptr);
        if (act == 0) {
            bool ok = send_saved(&arr[idx]);
            ir_toast(ok ? "Sent" : "Send failed", 500);
        } else if (act == 1) {
            char nm[16];
            snprintf(nm, sizeof(nm), "%s", arr[idx].name);
            if (k85_text_input("Name:", nm, nm, sizeof(nm)) && nm[0]) {
                name_clean(nm);
                snprintf(arr[idx].name, sizeof(arr[idx].name), "%s", nm);
                ir_rewrite(arr, n);
                ir_toast("Saved", 700);
            }
        } else if (act == 2) {
            for (int i = idx; i < n - 1; i++) arr[i] = arr[i + 1];
            n--;
            ir_rewrite(arr, n);
            ir_toast("Deleted", 700);
        }
    }
    heap_caps_free(arr);
}

// ---------- UI: IR TX ----------
static void run_ir_tv(void) {
    const char *bn[K85_BRAND_COUNT + 1];
    for (int i = 0; i < K85_BRAND_COUNT; i++) bn[i] = K85_BRANDS[i].name;
    bn[K85_BRAND_COUNT] = "Back";
    while (true) {
        int b = k85_run_list_menu("TV MODEL", bn, K85_BRAND_COUNT + 1, nullptr);
        if (b < 0 || b == K85_BRAND_COUNT) return;
        const K85IrBrand &br = K85_BRANDS[b];
        const char *btn[12];
        for (int j = 0; j < br.n; j++) btn[j] = br.btns[j].name;
        btn[br.n] = "Back";
        while (true) {
            int j = k85_run_list_menu(br.name, btn, br.n + 1, nullptr);
            if (j < 0 || j == br.n) break;
            bool ok = send_ir_code(br.proto, br.address, br.btns[j].command);
            ir_toast(ok ? "Sent" : "Send failed", 250);
        }
    }
}

static bool is_power_btn(const char *name) { return strncmp(name, "Power", 5) == 0; }

// true = A+B pressed (stop)
static bool blast_wait(int ms) {
    for (int w = 0; w < ms / 30; w++) {
        k85_input_update();
        if (k85_ab_held(300)) { k85_wait_ab_release(); return true; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return false;
}

static void run_ir_blast(bool everything) {
    K85IrSaved *arr = nullptr;
    int ns = 0;
    if (everything) {
        arr = saved_alloc();
        if (arr) ns = ir_load(arr, K85_IR_MAX_CODES);
    }
    int total = ns;
    for (int b = 0; b < K85_BRAND_COUNT; b++)
        for (int j = 0; j < K85_BRANDS[b].n; j++)
            if (everything || is_power_btn(K85_BRANDS[b].btns[j].name)) total++;

    int done = 0;
    bool stop = false;
    char m[112];
    for (int b = 0; b < K85_BRAND_COUNT && !stop; b++) {
        const K85IrBrand &br = K85_BRANDS[b];
        for (int j = 0; j < br.n && !stop; j++) {
            if (!everything && !is_power_btn(br.btns[j].name)) continue;
            done++;
            snprintf(m, sizeof(m), "BLAST %d/%d\n\n%s\n%s\n\nA+B = stop", done, total, br.name, br.btns[j].name);
            ir_msg(m);
            send_ir_code(br.proto, br.address, br.btns[j].command);
            stop = blast_wait(450);
        }
    }
    for (int i = 0; i < ns && !stop; i++) {
        done++;
        snprintf(m, sizeof(m), "BLAST %d/%d\n\nSaved:\n%s\n\nA+B = stop", done, total, arr[i].name);
        ir_msg(m);
        send_saved(&arr[i]);
        stop = blast_wait(450);
    }
    if (arr) heap_caps_free(arr);
    if (!stop) {
        ir_msg("Done.\n\nA+B = back");
        wait_ab_exit();
    }
}

static void run_ir_tx(void) {
    static const char *items[] = { "TV remote", "Blast power", "Blast all", "Back" };
    while (true) {
        int idx = k85_run_list_menu("IR TX", items, 4, nullptr);
        if (idx < 0 || idx == 3) return;
        if (idx == 0) run_ir_tv();
        else if (idx == 1) run_ir_blast(false);
        else run_ir_blast(true);
    }
}

// ---------- entry ----------
void k85_run_ir_remote(void) {
    if (!ir_rmt_init()) {
        k85_show_message("IR init failed");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    M5.Speaker.end();
    ir_enter_screen();

    static const char *items[] = { "IR TX", "IR RX", "Saved codes", "Back" };
    while (true) {
        int idx = k85_run_list_menu("IR REMOTE", items, 4, nullptr);
        if (idx < 0 || idx == 3) break;
        if (idx == 0) run_ir_tx();
        else if (idx == 1) run_ir_rx();
        else run_ir_saved();
    }

    ir_leave_screen();
    M5.Speaker.begin();
    k85_apply_sound_volume();
}