#include "mqtt_tool.h"
#include "k85_mqtt.h"
#include "common.h"
#include "theme.h"
#include "input.h"
#include "list_menu.h"
#include "text_input.h"
#include "config.h"
#include "esp_random.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// Запрашивает параметры брокера. broker_uri - обязательное поле в формате
// "mqtt://host:1883" или "mqtts://host:8883" (TLS определяется схемой).
// client_id/username/password можно оставить пустыми.
static bool run_broker_setup(void) {
    char uri[128] = "";
    snprintf(uri, sizeof(uri), "%s", g_config.mqtt_broker_uri);
    if (!k85_text_input("Broker URI (mqtt:// or mqtts://):", uri, uri, sizeof(uri)) || !uri[0]) return false;

    char client_id[32] = "";
    snprintf(client_id, sizeof(client_id), "%s", g_config.mqtt_client_id);
    if (!client_id[0]) snprintf(client_id, sizeof(client_id), "k85os-%04x", (unsigned)(esp_random() & 0xFFFF));
    k85_text_input("Client ID:", client_id, client_id, sizeof(client_id));

    char username[32] = "";
    snprintf(username, sizeof(username), "%s", g_config.mqtt_username);
    k85_text_input("Username (blank=none):", username, username, sizeof(username));

    char password[64] = "";
    if (username[0]) {
        k85_text_input("Password:", "", password, sizeof(password));
    }

    snprintf(g_config.mqtt_broker_uri, sizeof(g_config.mqtt_broker_uri), "%s", uri);
    snprintf(g_config.mqtt_client_id, sizeof(g_config.mqtt_client_id), "%s", client_id);
    snprintf(g_config.mqtt_username, sizeof(g_config.mqtt_username), "%s", username);
    snprintf(g_config.mqtt_password, sizeof(g_config.mqtt_password), "%s", password);
    k85_config_save();
    return true;
}

static bool try_connect(void) {
    k85_show_message("Connecting...");
    bool ok = k85_mqtt_connect(g_config.mqtt_broker_uri, g_config.mqtt_client_id,
                                g_config.mqtt_username, g_config.mqtt_password);
    if (!ok) {
        k85_show_message("Connect failed\n(check URI/creds)\nA+B=back");
        wait_ab_exit();
    }
    return ok;
}

static void run_publish(void) {
    char topic[96] = "";
    if (!k85_text_input("Topic:", "", topic, sizeof(topic)) || !topic[0]) return;

    char message[128] = "";
    if (!k85_text_input("Message:", "", message, sizeof(message))) return;

    bool ok = k85_mqtt_publish(topic, message, 0, false);
    k85_show_message(ok ? "Published!\nA+B=back" : "Publish failed\nA+B=back");
    wait_ab_exit();
}

static void run_subscribe(void) {
    char topic[96] = "";
    if (!k85_text_input("Topic to subscribe:", "", topic, sizeof(topic)) || !topic[0]) return;

    if (!k85_mqtt_subscribe(topic, 0)) {
        k85_show_message("Subscribe failed\nA+B=back");
        wait_ab_exit();
        return;
    }

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();
    int w = M5.Display.width();

    char last_topic[128] = "";
    char last_msg[256] = "";
    bool have_msg = false;

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); break; }

        if (k85_mqtt_has_new_message()) {
            k85_mqtt_get_last_message(last_topic, sizeof(last_topic), last_msg, sizeof(last_msg));
            have_msg = true;

            M5.Display.fillScreen(bg);
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(accent, bg);
            M5.Display.setCursor(4, 4);
            M5.Display.printf("Sub: %.*s", 30, topic);

            M5.Display.setTextColor(fg, bg);
            M5.Display.setCursor(4, 24);
            M5.Display.printf("From: %s", last_topic);
            M5.Display.setCursor(4, 40);
            M5.Display.print(last_msg);

            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.setCursor(4, M5.Display.height() - 12);
            M5.Display.print("A+B=back");
        } else if (!have_msg) {
            M5.Display.fillScreen(bg);
            M5.Display.setTextSize(1);
            M5.Display.setTextColor(accent, bg);
            M5.Display.setCursor(4, 4);
            M5.Display.printf("Sub: %.*s", 30, topic);
            M5.Display.setTextColor(fg, bg);
            M5.Display.setCursor(4, 24);
            M5.Display.print("Waiting for message...");
            M5.Display.setTextColor(0xAAAAAA, bg);
            M5.Display.setCursor(4, M5.Display.height() - 12);
            M5.Display.print("A+B=back");
            have_msg = true; // отрисовали заглушку один раз, дальше молча ждём
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    k85_mqtt_unsubscribe(topic);
}

void k85_run_mqtt_tool(void) {
    if (!g_config.mqtt_broker_uri[0]) {
        if (!run_broker_setup()) return;
    }

    if (!try_connect()) return;

    while (true) {
        static const char *items[] = {"Publish", "Subscribe & Listen", "Broker Settings", "Back"};
        int idx = k85_run_list_menu("MQTT", items, 4, nullptr);
        if (idx < 0 || idx == 3) {
            k85_mqtt_disconnect();
            return;
        }

        if (idx == 0) {
            run_publish();
        } else if (idx == 1) {
            run_subscribe();
        } else if (idx == 2) {
            k85_mqtt_disconnect();
            if (!run_broker_setup()) return;
            if (!try_connect()) return;
        }
    }
}
