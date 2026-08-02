#include "i2c_scanner.h"
#include "common.h"
#include "text_input.h"
#include "log.h"

#include "driver/i2c_master.h"
#include <cstdio>
#include <cstring>

struct KnownI2cDevice { uint8_t addr; const char *name; };

// Та же таблица, что и KNOWN_I2C_DEVICES в оригинале
static const KnownI2cDevice KNOWN_I2C_DEVICES[] = {
    {0x68, "MPU6886/MPU9250 (IMU)"}, {0x10, "BMM150 (Magneto)"},
    {0x75, "IP5306 (Power)"},        {0x6C, "SH200Q (IMU)"},
    {0x34, "AXP192 (Power)"},        {0x51, "BM8563/DS3231 (RTC)"},
    {0x38, "FT6336U (Touch)"},       {0x2E, "CHSC6540 (Touch)"},
    {0x18, "ES8311 (Audio)"},        {0x5C, "DHT12 (Temp/Hum)"},
    {0x77, "BMP280 (Baro)"},         {0x76, "BME280 (Env)"},
    {0x0C, "BMP388 (Baro)"},         {0x1E, "HMC5883L (Magneto)"},
    {0xEC, "SSD1306 (OLED)"},        {0x3C, "SSD1306 (OLED alt)"},
    {0x48, "ADS1115 (ADC)"},         {0x4A, "ADS1115 (ADC alt)"},
};
#define KNOWN_I2C_COUNT (int)(sizeof(KNOWN_I2C_DEVICES) / sizeof(KNOWN_I2C_DEVICES[0]))

static const char *known_i2c_name(uint8_t addr) {
    for (int i = 0; i < KNOWN_I2C_COUNT; i++)
        if (KNOWN_I2C_DEVICES[i].addr == addr) return KNOWN_I2C_DEVICES[i].name;
    return "Unknown";
}

void k85_run_i2c_scan(void) {
    // TODO: SDA=21/SCL=22 — как в оригинале (I2C(0, scl=22, sda=21)). Если у твоей платы
    // (ESP32-S3, не оригинальный ESP32 M5Stack Core) внутренняя шина IMU/RTC на других
    // пинах — поправь GPIO_NUM_21/22 ниже.
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = GPIO_NUM_1;  // TODO: подтверди реальный SDA для твоей платы
    bus_config.scl_io_num = GPIO_NUM_2;  // TODO: подтверди реальный SCL для твоей платы
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus_handle;
    if (i2c_new_master_bus(&bus_config, &bus_handle) != ESP_OK) {
        k85_log("I2C bus init failed");
        k85_show_message("I2C init failed\nA+B=back");
        return;
    }

    static char lines_buf[40][32];
    static const char *lines[40];
    int n_lines = 0;

    static uint8_t found_addrs[32];
    int found_n = 0;
    for (int addr = 1; addr < 127 && found_n < 32; addr++) {
        esp_err_t err = i2c_master_probe(bus_handle, addr, 50);
        if (err == ESP_OK) found_addrs[found_n++] = (uint8_t)addr;
    }
    i2c_del_master_bus(bus_handle);

    snprintf(lines_buf[n_lines], 32, "I2C Scan: %d found", found_n);
    lines[n_lines] = lines_buf[n_lines]; n_lines++;

    if (found_n == 0) {
        snprintf(lines_buf[n_lines], 32, "No devices found");
        lines[n_lines] = lines_buf[n_lines]; n_lines++;
    } else {
        for (int i = 0; i < found_n && n_lines < 40; i++) {
            snprintf(lines_buf[n_lines], 32, "0x%02X  %s", found_addrs[i], known_i2c_name(found_addrs[i]));
            lines[n_lines] = lines_buf[n_lines]; n_lines++;
        }
    }

    k85_area_show(lines, n_lines, "I2C SCANNER");
}


