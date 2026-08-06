#include "test_mode.h"
#include "common.h"
#include "input.h"
#include "list_menu.h"
#include "tools/i2c_scanner.h"
#include "tools/gpio_control.h"
#include "tools/color_test.h"
#include "tools/mic_test.h"
#include "tools/bt_scan.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_chip_info.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cmath>

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Куб с координатами акселерометра ----------
struct TmPoint3D { float x, y, z; };
static const TmPoint3D TM_CUBE_PTS[8] = {
    {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
    {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},
};
static const int TM_CUBE_EDGES[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

static TmPoint3D tm_rotate(const TmPoint3D &p, float ax, float ay) {
    float y2 = p.y * cosf(ax) - p.z * sinf(ax);
    float z2 = p.y * sinf(ax) + p.z * cosf(ax);
    float x3 = p.x * cosf(ay) + z2 * sinf(ay);
    float z3 = -p.x * sinf(ay) + z2 * cosf(ay);
    return {x3, y2, z3};
}

static void tm_project(const TmPoint3D &p, float scale, int cx, int cy, int &ox, int &oy) {
    const float fov = 4.0f;
    float f = fov / (fov + p.z);
    ox = cx + (int)(p.x * scale * f);
    oy = cy + (int)(p.y * scale * f);
}

static void run_test_cube(void) {
    float angle_x = 0.0f, angle_y = 0.0f;
    int cx = M5.Display.width() / 2;
    int cy = M5.Display.height() / 2 - 10;
    float scale = 26.0f;

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }

        float ax = 0, ay = 0, az = 0;
        M5.Imu.getAccel(&ax, &ay, &az);

        int px[8], py[8];
        for (int i = 0; i < 8; i++) {
            TmPoint3D rp = tm_rotate(TM_CUBE_PTS[i], angle_x, angle_y);
            tm_project(rp, scale, cx, cy, px[i], py[i]);
        }

        M5.Display.fillScreen(0x000000);
        for (int i = 0; i < 12; i++) {
            int a = TM_CUBE_EDGES[i][0], b = TM_CUBE_EDGES[i][1];
            M5.Display.drawLine(px[a], py[a], px[b], py[b], 0x00FFFF);
        }

        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0xFFFFFF, 0x000000);
        M5.Display.setCursor(4, 4);
        M5.Display.printf("X:%.2f Y:%.2f Z:%.2f", ax, ay, az);
        M5.Display.setCursor(4, M5.Display.height() - 12);
        M5.Display.print("A+B=back");

        angle_x += 0.02f + ay * 0.08f;
        angle_y += 0.015f + ax * 0.08f;

        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

// ---------- Тест поворота экрана ----------
static void run_rotation_test(void) {
    int rot = M5.Display.getRotation();
    while (true) {
        k85_input_update();
        M5.Display.fillScreen(0x000000);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(0xFFFFFF, 0x000000);
        M5.Display.setCursor(10, 10);
        M5.Display.printf("Rotation: %d", rot);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(10, 40);
        M5.Display.print("A=cycle B=apply&back");
        M5.Display.setCursor(10, 60);
        M5.Display.print("A+B=cancel");

        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed()) {
            rot = (rot + 1) % 4;
            M5.Display.setRotation(rot);
        }
        if (k85_btn_b_pressed()) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Тест яркости ----------
static void run_brightness_test(void) {
    int level = 50;
    M5.Display.setBrightness(level);
    while (true) {
        k85_input_update();
        M5.Display.fillScreen(0x000000);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(0xFFFFFF, 0x000000);
        M5.Display.setCursor(10, 10);
        M5.Display.printf("Brightness: %d%%", level);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(10, 40);
        M5.Display.print("A=+10%  A+B=back");

        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        if (k85_btn_a_pressed()) {
            level += 10;
            if (level > 100) level = 10;
            M5.Display.setBrightness(level);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Тест кнопок A/B ----------
static void run_button_test(void) {
    while (true) {
        k85_input_update();
        M5.Display.fillScreen(0x000000);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(k85_btn_a_is_down() ? 0x00FF00 : 0x555555, 0x000000);
        M5.Display.setCursor(10, 20);
        M5.Display.print("BUTTON A");
        M5.Display.setTextColor(k85_btn_b_is_down() ? 0x00FF00 : 0x555555, 0x000000);
        M5.Display.setCursor(10, 50);
        M5.Display.print("BUTTON B");
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0x777777, 0x000000);
        M5.Display.setCursor(10, 90);
        M5.Display.print("Hold BOTH 1s = exit");

        if (k85_ab_held(1000)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Сводка: батарея, температура, радио ----------
static temperature_sensor_handle_t s_tm_temp_handle = nullptr;
static bool s_tm_temp_ready = false;

static bool tm_init_temp(void) {
    if (s_tm_temp_ready) return true;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    if (temperature_sensor_install(&cfg, &s_tm_temp_handle) != ESP_OK) return false;
    if (temperature_sensor_enable(s_tm_temp_handle) != ESP_OK) return false;
    s_tm_temp_ready = true;
    return true;
}

static void run_system_check(void) {
    M5.Display.fillScreen(0x000000);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xFFFFFF, 0x000000);
    M5.Display.setCursor(4, 4);
    M5.Display.print("SYSTEM CHECK");

    int y = 20;
    char line[64];

    int32_t mv = M5.Power.getBatteryVoltage();
    snprintf(line, sizeof(line), "Battery: %s", mv > 0 ? "OK" : "N/A");
    M5.Display.setCursor(4, y); M5.Display.print(line); y += 14;
    if (mv > 0) {
        snprintf(line, sizeof(line), "  Voltage: %.2f V", mv / 1000.0f);
        M5.Display.setCursor(4, y); M5.Display.print(line); y += 14;
    }

    float temp_c = 0;
    bool has_temp = tm_init_temp() && temperature_sensor_get_celsius(s_tm_temp_handle, &temp_c) == ESP_OK;
    snprintf(line, sizeof(line), "Chip temp: %s", has_temp ? "OK" : "N/A");
    M5.Display.setCursor(4, y); M5.Display.print(line); y += 14;
    if (has_temp) {
        snprintf(line, sizeof(line), "  %.1f C", temp_c);
        M5.Display.setCursor(4, y); M5.Display.print(line); y += 14;
    }

    esp_chip_info_t info;
    esp_chip_info(&info);
    snprintf(line, sizeof(line), "Chip: OK (%d cores)", info.cores);
    M5.Display.setCursor(4, y); M5.Display.print(line); y += 14;

    y += 6;
    M5.Display.setCursor(4, y); M5.Display.print("Radio check running..."); y += 14;
    M5.Display.setTextColor(0x777777, 0x000000);
    M5.Display.setCursor(4, y + 20); M5.Display.print("A+B=back");

    uint8_t mac[6];
    bool wifi_ok = esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK;
    snprintf(line, sizeof(line), "WiFi radio: %s", wifi_ok ? "OK" : "FAIL");
    M5.Display.setTextColor(0xFFFFFF, 0x000000);
    M5.Display.setCursor(4, y); M5.Display.print(line);

    wait_ab_exit();
}

void k85_run_test_mode(void) {
    static const char *items[] = {
        "Cube (coords)", "Rotation test", "Brightness test",
        "Button A/B test", "System check (batt/temp/radio)",
        "Color Test", "Mic Test", "I2C Scanner", "GPIO Control", "Bluetooth Scan",
        "Back",
    };
    while (true) {
        int idx = k85_run_list_menu("TEST MODE", items, 11, nullptr);
        if (idx < 0 || idx == 10) return;
        if (idx == 0) run_test_cube();
        else if (idx == 1) run_rotation_test();
        else if (idx == 2) run_brightness_test();
        else if (idx == 3) run_button_test();
        else if (idx == 4) run_system_check();
        else if (idx == 5) k85_run_color_test();
        else if (idx == 6) k85_run_mic_test();
        else if (idx == 7) k85_run_i2c_scan();
        else if (idx == 8) k85_run_gpio_test();
        else if (idx == 9) k85_run_bt_scan();
    }
}


