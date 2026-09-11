#pragma once
#include <stdbool.h>
#include <stdint.h>
#define K85_BATTERY_MODE_COUNT 3
extern const char *k85_battery_modes[K85_BATTERY_MODE_COUNT];  // "Normal","Balanced","SuperEco"
#define K85_BRIGHTNESS_IDLE 15
#define K85_BRIGHTNESS_ECO  10

// Порог суммарного изменения ускорения (в единицах g) относительно значения
// на момент засыпания, при котором IMU считается "устройство сдвинули".
// Подобран на глаз - если будет слишком чувствительно (просыпается от
// вибрации стола) или наоборот вяло, поправить это число.
#define K85_SLEEP_IMU_THRESHOLD 0.15f

// Режимы пробуждения из Sleep (g_config.sleep_wake_mode)
#define K85_SLEEP_WAKE_BUTTON_A 0
#define K85_SLEEP_WAKE_IMU      1
#define K85_SLEEP_WAKE_BOTH     2
extern const char *k85_sleep_wake_mode_names[3]; // "Button A","IMU","Both"

// Инициализация (сбрасывает таймер активности)
void k85_power_init(void);
// Вызывать каждый цикл главного loop: проверяет idle-таймаут и гасит экран.
// Возвращает true, если экран только что притушен (событие для UI - показать lock screen).
bool k85_power_tick(void);
// Сбросить таймер простоя + вернуть яркость (вызывать при любом действии юзера)
void k85_wake_screen(void);
bool k85_is_dimmed(void);
// idle-таймаут в мс для текущего battery_mode_idx
uint32_t k85_idle_timeout_ms(void);

// ---------- Sleep / Power Off / Reboot ----------
// Блокирующий вызов: гасит экран и переводит WiFi в modem-sleep (если был
// активен - сама сеть не отключается, просто снижается энергопотребление),
// затем ждёт условия пробуждения согласно g_config.sleep_wake_mode (кнопка A,
// сдвиг по IMU относительно позиции на момент засыпания, или любое из двух).
// По возвращении экран и WiFi power-save восстановлены в исходное состояние.
void k85_enter_sleep(void);

// Программное "выключение": гасит экран и уходит в глубокий сон без таймера
// пробуждения - фактически до физического сброса/питания устройства.
// Не возвращается.
void k85_power_off(void);

// esp_restart() с логированием причины.
// Не возвращается.
void k85_power_reboot(void);