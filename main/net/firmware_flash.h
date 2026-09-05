#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define K85_FW_LIST_MAX 8
// Список .bin ассетов из latest-релиза kion85/k85OS. out_sig_urls - URL
// файла подписи (<name>.bin.sig) рядом с каждым .bin, пустая строка если
// подписи для этого файла в релизе нет (тогда установка будет отклонена).
bool k85_fwflash_list_available(char out_names[][64], char out_urls[][256], char out_sig_urls[][256], int max, int *out_count);
// Скачивает .bin и его .sig подпись, проверяет подпись, пишет ТОЛЬКО в
// свободный (неактивный) слот. НЕ активирует его. Без валидной подписи -
// гарантированный провал, слот не сохраняется.
typedef void (*k85_fwflash_progress_cb)(int percent);
bool k85_fwflash_from_url(const char *bin_url, const char *sig_url, k85_fwflash_progress_cb cb);
// Потоковая запись для приёма файла через веб-загрузку (без буферизации на диск).
// Вызывающий сам читает байты из сокета и передаёт их сюда по кускам.
bool k85_fwflash_stream_begin(void);
bool k85_fwflash_stream_write(const uint8_t *data, size_t len);
// signature_hex обязателен - 128 hex-символов (64 байта подписи Ed25519
// над SHA-256 хешем принятых данных). Без валидной подписи - провал,
// слот не сохраняется.
bool k85_fwflash_stream_end_verified(const char *signature_hex);
bool k85_fwflash_stream_end(void); // ВСЕГДА возвращает false - оставлено только для совместимости сигнатуры, используй _verified
void k85_fwflash_stream_abort(void); // прервать и откатить незавершённую запись
// Метка неактивного (свободного) слота - для отображения в UI.
const char *k85_fwflash_free_slot_label(void);