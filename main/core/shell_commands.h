#pragma once
#include <cstddef>
// Общий набор команд для локального терминала и SSH-сервера — один и тот
// же код, чтобы не дублировать логику. cmd_full — полная строка команды
// (может содержать аргумент через пробел, например "cat file.txt").
void k85_shell_run_command(const char *cmd_full, char *out, size_t out_size);