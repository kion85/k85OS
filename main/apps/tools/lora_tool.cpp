#include "lora_tool.h"
#include "list_menu.h"
#include "text_input.h"
#include "common.h"
#include "input.h"
#include "log.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// Регистр версии чипа SX127x. Для SX1276/77/78/79 (в т.ч. RFM95/96/97/98,
// M5Stack LoRa868 Unit) корректное чтение по SPI должно вернуть 0x12.
#define SX127X_REG_VERSION 0x42
#define K85_LORA_SPI_HOST SPI3_HOST

struct K85LoraPins {
    int sck = -1;
    int miso = -1;
    int mosi = -1;
    int cs = -1;
    int rst = -1;
    int dio0 = -1;
};

static K85LoraPins s_pins;
static bool s_bus_inited = false;
static spi_device_handle_t s_spi_dev = nullptr;

static bool pins_are_set() {
    return s_pins.sck >= 0 && s_pins.miso >= 0 && s_pins.mosi >= 0 &&
           s_pins.cs >= 0 && s_pins.rst >= 0;
}

static void lora_bus_deinit() {
    if (s_spi_dev) {
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = nullptr;
    }
    if (s_bus_inited) {
        spi_bus_free(K85_LORA_SPI_HOST);
        s_bus_inited = false;
    }
}

static bool lora_bus_init() {
    if (!pins_are_set()) return false;
    lora_bus_deinit();

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = s_pins.mosi;
    buscfg.miso_io_num = s_pins.miso;
    buscfg.sclk_io_num = s_pins.sck;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 64;

    if (spi_bus_initialize(K85_LORA_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        k85_log("lora: spi_bus_initialize failed (pins in use elsewhere?)");
        return false;
    }
    s_bus_inited = true;

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 1 * 1000 * 1000; // 1MHz - безопасно, SX127x тянет до 10MHz
    devcfg.mode = 0;
    devcfg.spics_io_num = s_pins.cs;
    devcfg.queue_size = 1;

    if (spi_bus_add_device(K85_LORA_SPI_HOST, &devcfg, &s_spi_dev) != ESP_OK) {
        k85_log("lora: spi_bus_add_device failed");
        lora_bus_deinit();
        return false;
    }

    if (s_pins.rst >= 0) {
        gpio_set_direction((gpio_num_t)s_pins.rst, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)s_pins.rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)s_pins.rst, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static bool lora_read_reg(uint8_t addr, uint8_t *out_val) {
    if (!s_spi_dev) return false;
    uint8_t tx[2] = { (uint8_t)(addr & 0x7F), 0x00 }; // MSB=0 -> чтение
    uint8_t rx[2] = {0};

    spi_transaction_t t = {};
    t.length = 16; // 2 байта = 16 бит
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    if (spi_device_transmit(s_spi_dev, &t) != ESP_OK) return false;
    *out_val = rx[1];
    return true;
}

static bool lora_detect_module() {
    if (!lora_bus_init()) return false;
    uint8_t version = 0;
    bool ok = lora_read_reg(SX127X_REG_VERSION, &version);
    lora_bus_deinit();
    return ok && version == 0x12;
}

static void ask_pin(const char *prompt, int *pin_out) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", *pin_out);
    if (k85_text_input(prompt, buf, buf, sizeof(buf))) *pin_out = atoi(buf);
}

static void set_pins_menu() {
    ask_pin("SCK pin:", &s_pins.sck);
    ask_pin("MISO pin:", &s_pins.miso);
    ask_pin("MOSI pin:", &s_pins.mosi);
    ask_pin("CS (NSS) pin:", &s_pins.cs);
    ask_pin("RST pin:", &s_pins.rst);
    ask_pin("DIO0 pin (opt, -1=skip):", &s_pins.dio0);
}

static void wait_ab_back() {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void show_pins_status() {
    char l0[32], l1[32], l2[32], l3[32], l4[32], l5[32];
    snprintf(l0, sizeof(l0), "SCK:  %d", s_pins.sck);
    snprintf(l1, sizeof(l1), "MISO: %d", s_pins.miso);
    snprintf(l2, sizeof(l2), "MOSI: %d", s_pins.mosi);
    snprintf(l3, sizeof(l3), "CS:   %d", s_pins.cs);
    snprintf(l4, sizeof(l4), "RST:  %d", s_pins.rst);
    snprintf(l5, sizeof(l5), "DIO0: %d", s_pins.dio0);
    const char *lines[6] = {l0, l1, l2, l3, l4, l5};
    k85_area_show(lines, 6, "LoRa Pins");
}

static void run_scan() {
    if (!pins_are_set()) {
        k85_show_message("Set pins first!\n(SCK/MISO/MOSI/CS/RST)\nA+B=back");
        wait_ab_back();
        return;
    }
    k85_show_message("Scanning for\nSX127x module...");
    vTaskDelay(pdMS_TO_TICKS(200));

    bool found = lora_detect_module();
    k85_show_message(found
        ? "Module FOUND!\nSX1276/77/78/79\nA+B=back"
        : "No module found\non these pins\nA+B=back");
    wait_ab_back();
}

void k85_run_lora_tool(void) {
    const char *items[] = {"Scan / Detect", "Set pins", "Show pins", "Back"};
    while (true) {
        int idx = k85_run_list_menu("LoRa (SX127x)", items, 4, nullptr);
        if (idx < 0 || idx == 3) return;
        if (idx == 0) run_scan();
        else if (idx == 1) set_pins_menu();
        else if (idx == 2) show_pins_status();
    }
}