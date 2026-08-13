#pragma once
#include <stdbool.h>
#include <stddef.h>

#define K85_APPREPO_MAX_ASSETS 16

// Получает список .thm файлов из релиза с тегом "uefi" в kion85/apps_k85os.
// out_names/out_urls — массивы max*[64]/[256], out_count — сколько реально нашли.
bool k85_apprepo_fetch_uefi_theme_list(char out_names[][64], char out_urls[][256], int max, int *out_count);

// Скачивает файл по URL и сохраняет в dest_path.
bool k85_apprepo_download_file(const char *url, const char *dest_path);
