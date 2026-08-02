#pragma once
#include <stdint.h>

#define K85_LOG_MAX 16
#define K85_LOG_LINE_LEN 64

void k85_log_init(void);

// Аналог log(msg) из MicroPython: "[Ns] msg"
void k85_log(const char *fmt, ...);

int k85_log_count(void);
// index 0 = самая старая запись из текущих в буфере
const char *k85_log_get(int index);
