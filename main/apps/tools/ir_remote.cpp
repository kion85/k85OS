#include "ir_remote.h"
#include "common.h"
#include "theme.h"
#include "battery.h"
#include "input.h"
#include "text_input.h"
#include "esp_log.h"
#include "list_menu.h"
#include "sound.h"

#include "M5Unified.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"

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
#define NEC_BIT_MARK 560
#define NEC_BIT_0_SPACE 560
#define NEC_BIT_1_SPACE 1690

#define SIRC_HEADER_MARK 2400
#define SIRC_HEADER_SPACE 600
#define SIRC_BIT_SPACE 600
#define SIRC_BIT_0_MARK 600
#define SIRC_BIT_1_MARK 1200

#define RC5_HALF_BIT 889

#define K85_IR_MAX_CODES 16
#define K85_IR_FILE "/flash/k85_ir_codes.txt"

struct K85IrCode {
    char name[16];
    uint16_t address;
    uint8_t command;
};

// ---------- Р‘СЂРµРЅРґРѕРІР°СЏ Р±Р°Р·Р° (РїСЂРѕРІРµСЂРµРЅРЅС‹Рµ РєРѕРґС‹, best-effort вЂ” РєР°Рє Сѓ Р»СЋР±РѕРіРѕ
// СѓРЅРёРІРµСЂСЃР°Р»СЊРЅРѕРіРѕ РїСѓР»СЊС‚Р°, РјРѕР¶РµС‚ РЅРµ РїРѕРґРѕР№С‚Рё Рє РєРѕРЅРєСЂРµС‚РЅРѕР№ РјРѕРґРµР»Рё) ----------
enum K85IrProtocol { IR_PROTO_NEC, IR_PROTO_SAMSUNG32, IR_PROTO_SIRC, IR_PROTO_NECEXT, IR_PROTO_RC5 };

struct K85IrBrandCode {
    const char *name;
    K85IrProtocol protocol;
    uint16_t address;
    uint16_t command;
};

static const K85IrBrandCode K85_IR_BRAND_DB[] = {
    {"Samsung (2014+)",     IR_PROTO_SAMSUNG32, 0x0007, 0x0002},
    {"Samsung (older)",     IR_PROTO_SAMSUNG32, 0x000E, 0x000C},
    {"LG / Haier (NEC)",    IR_PROTO_NEC,       0x0004, 0x0008},
    {"Sony (TV)",           IR_PROTO_SIRC,      0x0001, 0x0015},
    {"Sony (alt device)",   IR_PROTO_SIRC,      0x0010, 0x0015},
    {"Sony (alt Power)",    IR_PROTO_SIRC,      0x0001, 0x002F},
    {"Generic TV (NEC #2)", IR_PROTO_NEC,       0x0008, 0x0005},
    {"Generic TV (NECext A)", IR_PROTO_NECEXT,  0xDF00, 0x001C},
    {"Generic TV (NECext B)", IR_PROTO_NECEXT,  0x1818, 0x3FC0},
    {"Generic TV (NEC #3)",   IR_PROTO_NEC,     0x0008, 0x0017},
    {"Philips (RC5)",         IR_PROTO_RC5,     0x0000, 0x000C},
};
#define K85_IR_BRAND_COUNT (int)(sizeof(K85_IR_BRAND_DB) / sizeof(K85_IR_BRAND_DB[0]))

static rmt_channel_handle_t s_tx_chan = nullptr;
static rmt_channel_handle_t s_rx_chan = nullptr;
static rmt_encoder_handle_t s_copy_encoder = nullptr;
static bool s_rmt_inited = false;

static rmt_symbol_word_t s_rx_symbols[64];
static volatile bool s_rx_done = false;
static size_t s_rx_symbol_num = 0;

static bool rmt_rx_done_cb(rmt_channel_handle_t, const rmt_rx_done_event_data_t *edata, void *) {
    s_rx_symbol_num = edata->num_symbols;
    s_rx_done = true;
    return true;
}

static void start_rmt_receive(void) {
    rmt_receive_config_t cfg = {};
    cfg.signal_range_min_ns = 1000;
    cfg.signal_range_max_ns = 20000000;
    esp_err_t err = rmt_receive(s_rx_chan, s_rx_symbols, sizeof(s_rx_symbols), &cfg);
    if (err != ESP_OK) {
        // канал ещё не вернулся в ENABLE после предыдущего приёма (шум/гонка) —
        // просто пропускаем этот цикл, следующая попытка будет через vTaskDelay
    }
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

// РћР±С‰Р°СЏ NEC-С‚Р°Р№РјРёРЅРіРѕРІ СЃС…РµРјР° (РёСЃРїРѕР»СЊР·СѓРµС‚СЃСЏ Рё РґР»СЏ NEC, Рё РґР»СЏ Samsung32 вЂ” С‚Рµ Р¶Рµ
// РґР»РёС‚РµР»СЊРЅРѕСЃС‚Рё РёРјРїСѓР»СЊСЃРѕРІ, СЂР°Р·РЅРёС†Р° С‚РѕР»СЊРєРѕ РІ СЃРѕРґРµСЂР¶РёРјРѕРј 32-Р±РёС‚РЅРѕРіРѕ РєР°РґСЂР°).
static void encode_nec_family(uint32_t raw_data, rmt_symbol_word_t *symbols, size_t *symbol_count) {
    size_t idx = 0;
    symbols[idx].duration0 = NEC_HEADER_MARK; symbols[idx].level0 = 1;
    symbols[idx].duration1 = NEC_HEADER_SPACE; symbols[idx].level1 = 0;
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

// Samsung32: С‚Р° Р¶Рµ СЃС…РµРјР° С‚Р°Р№РјРёРЅРіРѕРІ, С‡С‚Рѕ Рё NEC, РЅРѕ 32 Р±РёС‚Р° = address(16) + command(16)
// Р±РµР· РёРЅРІРµСЂСЃРёРё СЃС‚Р°СЂС€РµРіРѕ Р±Р°Р№С‚Р° (РІ РѕС‚Р»РёС‡РёРµ РѕС‚ РєР»Р°СЃСЃРёС‡РµСЃРєРѕРіРѕ NEC).
static uint32_t samsung32_raw(uint16_t address, uint16_t command) {
    return ((uint32_t)command << 16) | (uint32_t)address;
}

// Sony SIRC: 12 Р±РёС‚ (7 Р±РёС‚ РєРѕРјР°РЅРґР° + 5 Р±РёС‚ Р°РґСЂРµСЃ), LSB first, header 2400/600,
// Р±РёС‚ '0' = mark 600/space 600, Р±РёС‚ '1' = mark 1200/space 600.
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

static const char *protocol_name(K85IrProtocol p) {
    switch (p) {
        case IR_PROTO_NEC: return "NEC";
        case IR_PROTO_SAMSUNG32: return "Samsung32";
        case IR_PROTO_SIRC: return "SIRC";
        case IR_PROTO_NECEXT: return "NECext";
        case IR_PROTO_RC5: return "RC5";
    }
    return "?";
}

// RC5 (Philips): Manchester-кодирование, 14 бит = 2 старт-бита(1,1) + toggle(0) + 5 бит адрес + 6 бит команда.
// Бит 0 = mark(889)+space(889), бит 1 = space(889)+mark(889) (по спецификации SB-Projects).
static void encode_rc5(uint16_t address, uint16_t command, rmt_symbol_word_t *symbols, size_t *symbol_count) {
    bool bits[14];
    bits[0] = true;  // start bit 1
    bits[1] = true;  // start bit 2
    bits[2] = false; // toggle (фиксированный, для одиночной посылки достаточно)
    for (int i = 0; i < 5; i++) bits[3 + i] = (address & (1 << (4 - i))) != 0;
    for (int i = 0; i < 6; i++) bits[8 + i] = (command & (1 << (5 - i))) != 0;

    size_t idx = 0;
    for (int i = 0; i < 14; i++) {
        if (bits[i]) {
            // логическая 1: space затем mark
            symbols[idx].duration0 = RC5_HALF_BIT; symbols[idx].level0 = 0;
            symbols[idx].duration1 = RC5_HALF_BIT; symbols[idx].level1 = 1;
        } else {
            // логический 0: mark затем space
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

static bool send_nec(uint16_t address, uint8_t command) {
    uint32_t raw = nec_raw(address, command);
    rmt_symbol_word_t symbols[68];
    size_t symbol_count = 0;
    encode_nec_family(raw, symbols, &symbol_count);
    return transmit_symbols(symbols, symbol_count);
}

static bool send_ir_brand_code(const K85IrBrandCode &code) {
    rmt_symbol_word_t symbols[68];
    size_t symbol_count = 0;

    switch (code.protocol) {
        case IR_PROTO_NEC: {
            uint32_t raw = nec_raw(code.address, (uint8_t)code.command);
            encode_nec_family(raw, symbols, &symbol_count);
            break;
        }
        case IR_PROTO_SAMSUNG32: {
            uint32_t raw = samsung32_raw(code.address, code.command);
            encode_nec_family(raw, symbols, &symbol_count);
            break;
        }
        case IR_PROTO_SIRC: {
            encode_sirc(code.address, code.command, symbols, &symbol_count);
            break;
        }
        case IR_PROTO_NECEXT: {
            uint32_t raw = samsung32_raw(code.address, code.command); // тот же принцип: прямой 32-битный кадр без инверсии
            encode_nec_family(raw, symbols, &symbol_count);
            break;
        }
        case IR_PROTO_RC5: {
            encode_rc5(code.address, code.command, symbols, &symbol_count);
            break;
        }
    }
    return transmit_symbols(symbols, symbol_count);
}

static bool decode_nec(rmt_symbol_word_t *symbols, uint32_t *out_raw) {
    *out_raw = 0;
    uint32_t header_low = symbols[0].duration0;
    uint32_t header_high = symbols[0].duration1;
    if (!(header_low > 8000 && header_high > 4000)) return false;

    for (int i = 0; i < 32; i++) {
        uint32_t mark = symbols[i + 1].duration0;
        uint32_t space = symbols[i + 1].duration1;
        if (mark < 300 || mark > 800) return false;
        if (space > 1000) *out_raw |= (1UL << i);
    }
    uint8_t cmd = (*out_raw >> 16) & 0xFF;
    uint8_t cmd_inv = (*out_raw >> 24) & 0xFF;
    if ((cmd ^ cmd_inv) != 0xFF) return false;
    return true;
}

// ---------- Generic noise-tolerant symbol comparison ----------
// Сравнивает два последовательных RMT-захвата: одиночный шум почти никогда
// не повторяется дважды с одинаковыми таймингами, а настоящий пульт при
// нажатии кнопки шлёт серию повторов — это и есть критерий "реальный сигнал".
static bool symbols_similar(rmt_symbol_word_t *a, size_t na, rmt_symbol_word_t *b, size_t nb) {
    if (na != nb || na < 4) return false;
    const int32_t TOL_US = 200;
    for (size_t i = 0; i < na; i++) {
        if (abs((int32_t)a[i].duration0 - (int32_t)b[i].duration0) > TOL_US) return false;
        if (abs((int32_t)a[i].duration1 - (int32_t)b[i].duration1) > TOL_US) return false;
    }
    return true;
}

// ---------- SIRC decode (для live-скана) ----------
static bool decode_sirc(rmt_symbol_word_t *symbols, size_t num, uint16_t *out_address, uint16_t *out_command) {
    if (num < 13) return false;
    uint32_t header_mark = symbols[0].duration0;
    uint32_t header_space = symbols[0].duration1;
    if (!(header_mark > 2000 && header_mark < 2800 && header_space > 400 && header_space < 800)) return false;
    uint16_t data = 0;
    for (int i = 0; i < 12; i++) {
        uint32_t mark = symbols[i + 1].duration0;
        if (mark > 900) data |= (1U << i);
    }
    *out_command = data & 0x7F;
    *out_address = (data >> 7) & 0x1F;
    return true;
}

// Ищет известную "функцию" (бренд + кнопку) по декодированному коду в базе.
static const K85IrBrandCode *match_brand(K85IrProtocol proto, uint16_t address, uint16_t command) {
    for (int i = 0; i < K85_IR_BRAND_COUNT; i++) {
        if (K85_IR_BRAND_DB[i].protocol == proto &&
            K85_IR_BRAND_DB[i].address == address &&
            K85_IR_BRAND_DB[i].command == command) {
            return &K85_IR_BRAND_DB[i];
        }
    }
    return nullptr;
}
// ---------- Storage ----------
static int load_codes(K85IrCode out[], int max_out) {
    FILE *f = fopen(K85_IR_FILE, "r");
    if (!f) return 0;
    int n = 0;
    char line[64];
    while (n < max_out && fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1 = 0;
        char *p2 = strchr(p1 + 1, '|');
        if (!p2) continue;
        *p2 = 0;
        snprintf(out[n].name, sizeof(out[n].name), "%.15s", line);
        out[n].address = (uint16_t)atoi(p1 + 1);
        out[n].command = (uint8_t)atoi(p2 + 1);
        n++;
    }
    fclose(f);
    return n;
}

static void save_codes(K85IrCode codes[], int count) {
    FILE *f = fopen(K85_IR_FILE, "w");
    if (!f) return;
    for (int i = 0; i < count; i++) {
        fprintf(f, "%s|%u|%u\n", codes[i].name, codes[i].address, codes[i].command);
    }
    fclose(f);
}

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- UI: Send ----------
static void run_ir_send(void) {
    K85IrCode codes[K85_IR_MAX_CODES];
    int n = load_codes(codes, K85_IR_MAX_CODES);
    if (n == 0) {
        k85_show_message("No saved codes\nA+B=back");
        wait_ab_exit();
        return;
    }
    const char *names[K85_IR_MAX_CODES];
    for (int i = 0; i < n; i++) names[i] = codes[i].name;

    int idx = k85_run_list_menu("SEND IR", names, n, nullptr);
    if (idx < 0) return;
    bool ok = send_nec(codes[idx].address, codes[idx].command);
    char msg[80];
    snprintf(msg, sizeof(msg), "NEC A:%04X C:%02X\n%s\nA+B=back",
             codes[idx].address, codes[idx].command, ok ? "Sent" : "Send failed");
    k85_show_message(msg);
    wait_ab_exit();
}

// ---------- UI: Brand codes ----------
static void run_ir_brand_codes(void) {
    const char *names[K85_IR_BRAND_COUNT + 1];
    for (int i = 0; i < K85_IR_BRAND_COUNT; i++) names[i] = K85_IR_BRAND_DB[i].name;
    names[K85_IR_BRAND_COUNT] = "Back";

    while (true) {
        int idx = k85_run_list_menu("TV BRAND POWER", names, K85_IR_BRAND_COUNT + 1, nullptr);
        if (idx < 0 || idx == K85_IR_BRAND_COUNT) return;

        bool ok = send_ir_brand_code(K85_IR_BRAND_DB[idx]);
        char msg[100];
        snprintf(msg, sizeof(msg), "%s\nA:%04X C:%04X\n%s\nA+B=back",
                 protocol_name(K85_IR_BRAND_DB[idx].protocol),
                 K85_IR_BRAND_DB[idx].address, K85_IR_BRAND_DB[idx].command,
                 ok ? "Sent" : "Send failed");
        k85_show_message(msg);
        wait_ab_exit();
    }
}

// ---------- UI: Blast all (TV-B-Gone style) ----------
static void run_ir_blast_all(void) {
    k85_show_message("Blasting codes...\nA+B=stop");
    for (int i = 0; i < K85_IR_BRAND_COUNT; i++) {
        k85_input_update();
        if (k85_ab_held(300)) { k85_wait_ab_release(); return; }

        char msg[120];
        snprintf(msg, sizeof(msg), "Blasting %d/%d\n%.30s\n%s A:%04X C:%04X\nA+B=stop",
                 i + 1, K85_IR_BRAND_COUNT, K85_IR_BRAND_DB[i].name,
                 protocol_name(K85_IR_BRAND_DB[i].protocol),
                 K85_IR_BRAND_DB[i].address, K85_IR_BRAND_DB[i].command);
        k85_show_message(msg);

        send_ir_brand_code(K85_IR_BRAND_DB[i]);

        for (int w = 0; w < 15; w++) {
            k85_input_update();
            if (k85_ab_held(300)) { k85_wait_ab_release(); return; }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    k85_show_message("Blast complete\nA+B=back");
    wait_ab_exit();
}

// ---------- UI: Learn ----------
static void run_ir_learn(void) {
    char name[16] = "";
    if (!k85_text_input("Name for code:", "", name, sizeof(name))) return;
    if (strlen(name) == 0) return;

    k85_show_message("Point remote here\nand press button\nA+B=cancel");
    start_rmt_receive();

    bool got = false;
    uint32_t decoded_raw = 0;

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }

        if (s_rx_done) {
            s_rx_done = false;
            ESP_LOGI("k85_ir", "RX got %d symbols", (int)s_rx_symbol_num);
            for (size_t i = 0; i < s_rx_symbol_num && i < 8; i++) {
                ESP_LOGI("k85_ir", "  [%d] mark=%u space=%u", (int)i,
                         (unsigned)s_rx_symbols[i].duration0, (unsigned)s_rx_symbols[i].duration1);
            }
            if (decode_nec(s_rx_symbols, &decoded_raw)) {
                got = true;
                break;
            }
            start_rmt_receive();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (!got) return;

    uint16_t address = decoded_raw & 0xFFFF;
    uint8_t command = (decoded_raw >> 16) & 0xFF;

    K85IrCode codes[K85_IR_MAX_CODES];
    int n = load_codes(codes, K85_IR_MAX_CODES);
    if (n >= K85_IR_MAX_CODES) n = K85_IR_MAX_CODES - 1;
    snprintf(codes[n].name, sizeof(codes[n].name), "%s", name);
    codes[n].address = address;
    codes[n].command = command;
    n++;
    save_codes(codes, n);

    char buf[64];
    snprintf(buf, sizeof(buf), "Saved: %s\nA+B=back", name);
    k85_show_message(buf);
    wait_ab_exit();
}

// ---------- UI: Live Scan — шум игнорируется, на экран выводится только
// сигнал, повторившийся 2 раза подряд с одинаковыми таймингами (так делает
// настоящий пульт при нажатии кнопки). A+B — выход.
static void run_ir_live_scan(void) {
    M5.Display.fillScreen(0x000000);
    M5.Display.setTextSize(1);

    rmt_symbol_word_t prev_symbols[64];
    size_t prev_count = 0;
    bool have_prev = false;
    uint32_t total_seen = 0;
    uint32_t last_heartbeat = 0;

    char result_line1[64] = "Пульт ещё не пойман";
    char result_line2[64] = "";
    char result_line3[64] = "";

    auto redraw = [&]() {
        M5.Display.fillScreen(0x000000);
        M5.Display.setTextColor(0x07FF, 0x000000);
        M5.Display.setCursor(4, 4);
        M5.Display.println("Ожидание сигнала...");
        M5.Display.setCursor(4, 20);
        M5.Display.printf("Захватов: %lu", (unsigned long)total_seen);

        M5.Display.setTextColor(0x07E0, 0x000000);
        M5.Display.setCursor(4, 60);
        M5.Display.println(result_line1);
        M5.Display.setCursor(4, 76);
        M5.Display.println(result_line2);
        M5.Display.setCursor(4, 92);
        M5.Display.println(result_line3);

        M5.Display.setTextColor(0xFFFF, 0x000000);
        M5.Display.setCursor(4, 130);
        M5.Display.println("A+B = выход");
    };

    redraw();
    start_rmt_receive();

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }

        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - last_heartbeat > 1000) {
            last_heartbeat = now;
            redraw();
        }

        if (s_rx_done) {
            s_rx_done = false;
            size_t cur_count = s_rx_symbol_num;
            total_seen++;

            bool is_real = have_prev && symbols_similar(prev_symbols, prev_count, s_rx_symbols, cur_count);

            if (is_real) {
                uint32_t nec_raw_val = 0;
                uint16_t sirc_addr = 0, sirc_cmd = 0;

                if (cur_count >= 33 && decode_nec(s_rx_symbols, &nec_raw_val)) {
                    uint16_t addr = nec_raw_val & 0xFFFF;
                    uint8_t cmd = (nec_raw_val >> 16) & 0xFF;
                    const K85IrBrandCode *m = match_brand(IR_PROTO_NEC, addr, cmd);
                    if (!m) m = match_brand(IR_PROTO_SAMSUNG32, addr, cmd);
                    snprintf(result_line1, sizeof(result_line1), "Протокол: NEC/Samsung");
                    snprintf(result_line2, sizeof(result_line2), "Код: A:%04X C:%02X", addr, cmd);
                    snprintf(result_line3, sizeof(result_line3), "Функция: %s", m ? m->name : "неизвестна");
                } else if (decode_sirc(s_rx_symbols, cur_count, &sirc_addr, &sirc_cmd)) {
                    const K85IrBrandCode *m = match_brand(IR_PROTO_SIRC, sirc_addr, sirc_cmd);
                    snprintf(result_line1, sizeof(result_line1), "Протокол: Sony SIRC");
                    snprintf(result_line2, sizeof(result_line2), "Код: A:%02X C:%02X", sirc_addr, sirc_cmd);
                    snprintf(result_line3, sizeof(result_line3), "Функция: %s", m ? m->name : "неизвестна");
                } else {
                    snprintf(result_line1, sizeof(result_line1), "Протокол: неизвестен");
                    snprintf(result_line2, sizeof(result_line2), "Импульсов: %u", (unsigned)cur_count);
                    snprintf(result_line3, sizeof(result_line3), "Сигнал реальный (повтор совпал)");
                }
                redraw();
            }

            memcpy(prev_symbols, s_rx_symbols, cur_count * sizeof(rmt_symbol_word_t));
            prev_count = cur_count;
            have_prev = true;

            start_rmt_receive();
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
// ---------- UI: Delete ----------
static void run_ir_delete(void) {
    K85IrCode codes[K85_IR_MAX_CODES];
    int n = load_codes(codes, K85_IR_MAX_CODES);
    if (n == 0) {
        k85_show_message("No saved codes\nA+B=back");
        wait_ab_exit();
        return;
    }
    const char *names[K85_IR_MAX_CODES];
    for (int i = 0; i < n; i++) names[i] = codes[i].name;

    int idx = k85_run_list_menu("DELETE IR", names, n, nullptr);
    if (idx < 0) return;

    for (int i = idx; i < n - 1; i++) codes[i] = codes[i + 1];
    n--;
    save_codes(codes, n);
    k85_show_message("Deleted\nA+B=back");
    wait_ab_exit();
}


// ---------- UI: Edit ----------
static bool run_ir_edit_code(K85IrCode *code) {
    char name[16];
    snprintf(name, sizeof(name), "%s", code->name);
    if (!k85_text_input("Name:", name, name, sizeof(name))) return false;
    if (strlen(name) == 0) return false;

    char addr_str[8];
    snprintf(addr_str, sizeof(addr_str), "%04X", code->address);
    if (!k85_text_input("Address (hex):", addr_str, addr_str, sizeof(addr_str))) return false;

    char cmd_str[8];
    snprintf(cmd_str, sizeof(cmd_str), "%02X", code->command);
    if (!k85_text_input("Command (hex):", cmd_str, cmd_str, sizeof(cmd_str))) return false;

    snprintf(code->name, sizeof(code->name), "%s", name);
    code->address = (uint16_t)strtoul(addr_str, nullptr, 16);
    code->command = (uint8_t)strtoul(cmd_str, nullptr, 16);
    return true;
}

// ---------- UI: Saved signals (Send/Edit/Delete) ----------
static void run_ir_saved_signals(void) {
    while (true) {
        K85IrCode codes[K85_IR_MAX_CODES];
        int n = load_codes(codes, K85_IR_MAX_CODES);
        if (n == 0) {
            k85_show_message("No saved codes\nA+B=back");
            wait_ab_exit();
            return;
        }
        const char *names[K85_IR_MAX_CODES];
        for (int i = 0; i < n; i++) names[i] = codes[i].name;

        int idx = k85_run_list_menu("SAVED SIGNALS", names, n, nullptr);
        if (idx < 0) return;

        static const char *actions[] = {"Send", "Edit", "Delete", "Back"};
        int act = k85_run_list_menu(codes[idx].name, actions, 4, nullptr);
        if (act < 0 || act == 3) continue;

        if (act == 0) {
            bool ok = send_nec(codes[idx].address, codes[idx].command);
            char msg[80];
            snprintf(msg, sizeof(msg), "NEC A:%04X C:%02X\n%s\nA+B=back",
                     codes[idx].address, codes[idx].command, ok ? "Sent" : "Send failed");
            k85_show_message(msg);
            wait_ab_exit();
        } else if (act == 1) {
            if (run_ir_edit_code(&codes[idx])) {
                save_codes(codes, n);
                k85_show_message("Saved\nA+B=back");
                wait_ab_exit();
            }
        } else if (act == 2) {
            for (int i = idx; i < n - 1; i++) codes[i] = codes[i + 1];
            n--;
            save_codes(codes, n);
            k85_show_message("Deleted\nA+B=back");
            wait_ab_exit();
        }
    }
}
void k85_run_ir_remote(void) {
    if (!ir_rmt_init()) {
        k85_show_message("IR init failed");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    M5.Speaker.end();

    static const char *items[] = {"Saved Signals", "Learn new (receive)", "TV Brand Power", "Blast all (TV-B-Gone)", "Back"};
    while (true) {
        int idx = k85_run_list_menu("IR REMOTE", items, 5, nullptr);
        if (idx < 0 || idx == 4) break;
        if (idx == 0) run_ir_saved_signals();
        else if (idx == 1) run_ir_learn();
        else if (idx == 2) run_ir_brand_codes();
        else if (idx == 3) run_ir_blast_all();
    }

    M5.Speaker.begin();
    k85_apply_sound_volume();
}







