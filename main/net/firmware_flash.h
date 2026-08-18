#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define K85_FW_LIST_MAX 8

// Список .bin ассетов из latest-релиза kion85/k85OS.
bool k85_fwflash_list_available(char out_names[][64], char out_urls[][256], int max, int *out_count);

// Скачивает .bin по URL и пишет ТОЛЬКО в свободный (неактивный) слот. НЕ активирует его.
typedef void (*k85_fwflash_progress_cb)(int percent);
bool k85_fwflash_from_url(const char *url, k85_fwflash_progress_cb cb);

// Потоковая запись для приёма файла через веб-загрузку (без буферизации на диск).
// Вызывающий сам читает байты из сокета и передаёт их сюда по кускам.
bool k85_fwflash_stream_begin(void);
bool k85_fwflash_stream_write(const uint8_t *data, size_t len);
bool k85_fwflash_stream_end(void); // true = успех, слот записан, НЕ активирован
void k85_fwflash_stream_abort(void); // прервать и откатить незавершённую запись

// Метка неактивного (свободного) слота — для отображения в UI.
const char *k85_fwflash_free_slot_label(void);
