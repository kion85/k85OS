#include "k85_mqtt.h"
#include "log.h"

#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <cstring>
#include <cstdio>

static esp_mqtt_client_handle_t s_client = nullptr;
static bool s_connected = false;
static EventGroupHandle_t s_evt_group = nullptr;
static const int MQTT_CONNECTED_BIT = BIT0;
static const int MQTT_ERROR_BIT = BIT1;

static char s_last_topic[128] = "";
static char s_last_message[256] = "";
static bool s_has_new_message = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            k85_log("mqtt: connected");
            if (s_evt_group) xEventGroupSetBits(s_evt_group, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            k85_log("mqtt: disconnected");
            break;
        case MQTT_EVENT_ERROR:
            k85_log("mqtt: error event");
            if (s_evt_group) xEventGroupSetBits(s_evt_group, MQTT_ERROR_BIT);
            break;
        case MQTT_EVENT_DATA: {
            int tlen = event->topic_len < (int)sizeof(s_last_topic) - 1 ? event->topic_len : (int)sizeof(s_last_topic) - 1;
            int dlen = event->data_len < (int)sizeof(s_last_message) - 1 ? event->data_len : (int)sizeof(s_last_message) - 1;
            memcpy(s_last_topic, event->topic, tlen);
            s_last_topic[tlen] = 0;
            memcpy(s_last_message, event->data, dlen);
            s_last_message[dlen] = 0;
            s_has_new_message = true;
            k85_log("mqtt: data on %s (%d bytes)", s_last_topic, dlen);
            break;
        }
        default:
            break;
    }
}

bool k85_mqtt_connect(const char *broker_uri, const char *client_id,
                       const char *username, const char *password) {
    if (s_client) {
        k85_mqtt_disconnect();
    }
    if (!s_evt_group) s_evt_group = xEventGroupCreate();
    xEventGroupClearBits(s_evt_group, MQTT_CONNECTED_BIT | MQTT_ERROR_BIT);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = broker_uri;
    // Схема "mqtts://" сама переключает esp-mqtt на TLS; bundle нужен только
    // для проверки серверного сертификата в этом случае - для plain "mqtt://"
    // это поле просто игнорируется.
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;

    if (client_id && client_id[0]) cfg.credentials.client_id = client_id;
    if (username && username[0]) cfg.credentials.username = username;
    if (password && password[0]) cfg.credentials.authentication.password = password;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        k85_log("mqtt: client_init failed");
        return false;
    }

    esp_mqtt_client_register_event(s_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, nullptr);

    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        k85_log("mqtt: client_start failed");
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_evt_group, MQTT_CONNECTED_BIT | MQTT_ERROR_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_CONNECTED_BIT)) {
        k85_log("mqtt: connect timeout/error");
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        s_connected = false;
        return false;
    }
    return true;
}

void k85_mqtt_disconnect(void) {
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
    }
    s_connected = false;
}

bool k85_mqtt_is_connected(void) { return s_connected; }

bool k85_mqtt_publish(const char *topic, const char *message, int qos, bool retain) {
    if (!s_client || !s_connected) return false;
    int msg_id = esp_mqtt_client_publish(s_client, topic, message, 0, qos, retain ? 1 : 0);
    return msg_id >= 0;
}

bool k85_mqtt_subscribe(const char *topic, int qos) {
    if (!s_client || !s_connected) return false;
    int msg_id = esp_mqtt_client_subscribe(s_client, topic, qos);
    return msg_id >= 0;
}

void k85_mqtt_unsubscribe(const char *topic) {
    if (!s_client || !s_connected) return;
    esp_mqtt_client_unsubscribe(s_client, topic);
}

bool k85_mqtt_get_last_message(char *topic_out, size_t topic_size,
                                char *msg_out, size_t msg_size) {
    if (s_last_topic[0] == 0) return false;
    if (topic_out) snprintf(topic_out, topic_size, "%s", s_last_topic);
    if (msg_out) snprintf(msg_out, msg_size, "%s", s_last_message);
    s_has_new_message = false;
    return true;
}

bool k85_mqtt_has_new_message(void) { return s_has_new_message; }
