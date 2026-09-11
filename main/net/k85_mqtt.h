#pragma once
#include <stddef.h>

// Обёртка над esp-mqtt. Поддерживает и mqtt://, и mqtts:// (TLS) в одном
// URI - схема определяет протокол автоматически. Для TLS используется тот
// же сертификатный bundle, что и для OTA/HTTPS (esp_crt_bundle_attach) -
// покрывает публичные CA (HiveMQ Cloud, test.mosquitto.org и т.п.).

// Подключается к брокеру. broker_uri пример: "mqtt://host:1883" или
// "mqtts://host:8883". username/password можно передать nullptr/"" если
// брокер их не требует. Блокирует до CONNECTED/ошибки или таймаута (10с).
bool k85_mqtt_connect(const char *broker_uri, const char *client_id,
                       const char *username, const char *password);

void k85_mqtt_disconnect(void);
bool k85_mqtt_is_connected(void);

bool k85_mqtt_publish(const char *topic, const char *message, int qos, bool retain);
bool k85_mqtt_subscribe(const char *topic, int qos);
void k85_mqtt_unsubscribe(const char *topic);

// Последнее полученное сообщение по подписке (если было). Возвращает false,
// если сообщений ещё не было. topic_out/msg_out можно передать nullptr, если
// не нужны (например, чтобы просто проверить "было ли что-то новое").
bool k85_mqtt_get_last_message(char *topic_out, size_t topic_size,
                                char *msg_out, size_t msg_size);

// true, если с последнего вызова get_last_message пришло новое сообщение.
bool k85_mqtt_has_new_message(void);
