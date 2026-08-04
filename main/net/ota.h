#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Проверяет GitHub Releases на новую версию.
// Если найдена — заполняет out_version/out_url и возвращает true.
bool k85_ota_check_update(char *out_version, size_t ver_size, char *out_url, size_t url_size);

// Скачивает и прошивает bin по URL, затем делает restart.
// Вызывает progress_cb(percent) периодически, если не nullptr.
typedef void (*k85_ota_progress_cb)(int percent);
bool k85_ota_perform_update(const char *url, k85_ota_progress_cb progress_cb);

// Запускает фоновую задачу, которая раз в интервал (мс) проверяет обновления
// и, если найдено, шлёт уведомление через notifications.h
void k85_ota_start_background_check(uint32_t interval_ms);

