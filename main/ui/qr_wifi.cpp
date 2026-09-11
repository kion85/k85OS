#include "qr_wifi.h"
#include "input.h"
#include "qrcode.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

// esp_qrcode_generate() принимает только один колбэк без пользовательского
// контекста, поэтому единственный способ передать что-либо в display_func -
// статическая переменная, заполняемая непосредственно перед вызовом generate.
// Так как весь показ QR синхронный и блокирующий (никакой параллельности),
// это безопасно.
static void render_qrcode_to_display(esp_qrcode_handle_t qrcode) {
    int size = esp_qrcode_get_size(qrcode);
    if (size <= 0) return;

    int screen_w = M5.Display.width();
    int screen_h = M5.Display.height();
    int margin = 16;
    int avail = (screen_w < screen_h ? screen_w : screen_h) - margin;
    int scale = avail / size;
    if (scale < 1) scale = 1;
    int qr_px = scale * size;
    int ox = (screen_w - qr_px) / 2;
    int oy = (screen_h - qr_px) / 2;

    // Белый фон обязателен - большинство сканеров плохо читают QR в тёмной
    // теме устройства (инвертированный QR не по стандарту).
    M5.Display.fillScreen(0xFFFFFF);
    M5.Display.fillRect(ox - 4, oy - 4, qr_px + 8, qr_px + 8, 0xFFFFFF);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                M5.Display.fillRect(ox + x * scale, oy + y * scale, scale, scale, 0x000000);
            }
        }
    }

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x000000, 0xFFFFFF);
    M5.Display.setCursor(4, screen_h - 10);
    M5.Display.print("A+B=back");
}

void k85_show_wifi_qr(const char *ssid, const char *password, bool open) {
    char payload[160];
    if (open || !password || !password[0]) {
        snprintf(payload, sizeof(payload), "WIFI:S:%s;T:nopass;;", ssid);
    } else {
        snprintf(payload, sizeof(payload), "WIFI:S:%s;T:WPA;P:%s;;", ssid, password);
    }

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = render_qrcode_to_display;
    cfg.max_qrcode_version = 10;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

    esp_err_t err = esp_qrcode_generate(&cfg, payload);
    if (err != ESP_OK) {
        // Не должно происходить для коротких SSID/паролей k85OS, но на
        // всякий случай не залипаем на пустом/чёрном экране.
        M5.Display.fillScreen(0x000000);
        M5.Display.setTextColor(0xFF0000, 0x000000);
        M5.Display.setCursor(10, 40);
        M5.Display.print("QR generation failed\nA+B=back");
    }

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}