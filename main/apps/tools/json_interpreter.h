#pragma once
// Интерпретатор простых JSON-игр (текстовые квесты с ветвлением и флагами).
// Пользователь пишет .json файл по схеме screens/choices/set/need, кладёт
// его через веб-файлменеджер в /littlefs/games_json/, выбирает из списка
// здесь - и игра запускается.
void k85_run_json_interpreter(void);
// K85STORE1: run one .json game file directly (full path)
void k85_json_run_file(const char *path);