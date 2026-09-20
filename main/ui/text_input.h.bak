#pragma once
#include <cstddef>

// Возвращает true если пользователь подтвердил ввод (OK либо A+B held), false если Exit.
// Результат кладётся в out (обрезается по out_size). initial — стартовый текст (можно "").
bool k85_text_input(const char *prompt, const char *initial, char *out, size_t out_size);

// Простой экран из нескольких строк текста с заголовком, ждёт A+B для выхода.
void k85_area_show(const char *const lines[], int count, const char *title);
