#pragma once
#include <cstddef>

// Возвращает true если пользователь подтвердил ввод (OK либо A+B held), false если Exit.
// Результат кладётся в out (обрезается по out_size). initial — стартовый текст (можно "").
bool k85_text_input(const char *prompt, const char *initial, char *out, size_t out_size);

// Многострочный редактор той же клавиатурой (A=след. клавиша, hold A=след.
// строка клавиатуры, B=выбрать, A+B=сохранить и выйти). Добавлена клавиша
// ENTER (вставляет перенос строки в текст). Верхняя область показывает
// последние строки буфера - при наборе длинного текста видимая часть
// естественно "сдвигается вниз", как обычный многострочный ввод.
// initial - исходное содержимое (можно многострочным, "" для нового файла).
// Возвращает true при сохранении (OK/A+B held), false при Exit (изменения отбрасываются).
bool k85_text_input_multiline(const char *prompt, const char *initial, char *out, size_t out_size);

// Простой экран из нескольких строк текста с заголовком, ждёт A+B для выхода.
void k85_area_show(const char *const lines[], int count, const char *title);
