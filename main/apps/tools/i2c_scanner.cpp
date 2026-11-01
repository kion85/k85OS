#pragma GCC diagnostic ignored "-Wformat-truncation"
// K85I2C1: the scanner uses the I2C buses that M5Unified already owns (Grove port and the internal bus),
// so it works on any board without knowing the pins.
#include "i2c_scanner.h"
#include "common.h"
#include "text_input.h"
#include "list_menu.h"
#include "input.h"
#include "log.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

struct KnownI2cDevice { uint8_t addr; const char *name; };

// several chips share some addresses, so the first match is only a hint
static const KnownI2cDevice KNOWN_I2C_DEVICES[] = {
    {0x18, "ES8311 audio codec"},     {0x1D, "ADXL345 (accel)"},
    {0x20, "PCF8574 / MCP23017"},     {0x23, "BH1750 (light)"},
    {0x29, "VL53L0X / TCS34725"},     {0x38, "AHT20 / FT6336U"},
    {0x39, "APDS-9960"},              {0x3C, "SSD1306 (OLED)"},
    {0x3D, "SSD1306 (OLED alt)"},     {0x40, "INA219 / SI7021"},
    {0x44, "SHT3x"},                  {0x45, "SHT3x alt"},
    {0x48, "ADS1115 / PCF8591"},      {0x4A, "ADS1115 alt"},
    {0x50, "EEPROM (AT24C)"},         {0x51, "BM8563 / PCF8563 (RTC)"},
    {0x53, "ADXL345 alt"},            {0x57, "MAX30102 / EEPROM"},
    {0x5A, "MLX90614 / CCS811"},      {0x62, "SCD4x (CO2)"},
    {0x68, "IMU / DS3231 (RTC)"},     {0x69, "IMU alt (BMI270)"},
    {0x70, "TCA9548A (mux)"},         {0x76, "BME280 / BMP280"},
    {0x77, "BME280 / BMP388"},
};
#define KNOWN_I2C_COUNT (int)(sizeof(KNOWN_I2C_DEVICES) / sizeof(KNOWN_I2C_DEVICES[0]))

static const char *known_i2c_name(uint8_t addr) {
    for (int i = 0; i < KNOWN_I2C_COUNT; i++)
        if (KNOWN_I2C_DEVICES[i].addr == addr) return KNOWN_I2C_DEVICES[i].name;
    return "unknown";
}

static void wait_ab(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

template <typename BUS>
static void scan_bus(BUS &bus, const char *title) {
    bool was_enabled = bus.isEnabled();
    if (!was_enabled && !bus.begin()) {
        k85_log("I2C bus init failed (%s)", title);
        k85_show_message("I2C bus not available\nA+B=back");
        wait_ab();
        return;
    }

    static char lines_buf[40][32];
    static const char *lines[40];
    int n_lines = 0;
    static uint8_t found[40];
    int found_n = 0;

    for (int addr = 0x08; addr <= 0x77 && found_n < 38; addr++) {
        if (bus.scanID((uint8_t)addr, 100000)) found[found_n++] = (uint8_t)addr;
    }
    int sda = (int)bus.getSDA(), scl = (int)bus.getSCL();
    if (!was_enabled) bus.release();

    char k85_tmp[64];   // K85I2C2: build the line in a larger buffer, then cut it to the screen width
    snprintf(k85_tmp, sizeof(k85_tmp), "SDA:%d SCL:%d  found:%d", sda, scl, found_n);
    snprintf(lines_buf[n_lines], 32, "%.31s", k85_tmp);
    lines[n_lines] = lines_buf[n_lines]; n_lines++;
    if (found_n == 0) {
        snprintf(lines_buf[n_lines], 32, "No devices found");
        lines[n_lines] = lines_buf[n_lines]; n_lines++;
    } else {
        for (int i = 0; i < found_n && n_lines < 40; i++) {
            snprintf(lines_buf[n_lines], 32, "0x%02X  %.22s", found[i], known_i2c_name(found[i]));
            lines[n_lines] = lines_buf[n_lines]; n_lines++;
        }
    }
    k85_area_show(lines, n_lines, title);
}

void k85_run_i2c_scan(void) {
    static const char *items[] = { "Grove port (A)", "Internal bus", "Back" };
    while (true) {
        int idx = k85_run_list_menu("I2C SCANNER", items, 3, nullptr);
        if (idx < 0 || idx == 2) return;
        k85_show_message("Scanning...");
        if (idx == 0) scan_bus(M5.Ex_I2C, "GROVE PORT");
        else scan_bus(M5.In_I2C, "INTERNAL BUS");
    }
}