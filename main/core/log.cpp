#include "log.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "esp_timer.h"

static char s_lines[K85_LOG_MAX][K85_LOG_LINE_LEN];
static int s_count = 0;       // сколько реально заполнено (до K85_LOG_MAX)
static int s_head = 0;        // индекс следующей записи для перезаписи
static int64_t s_boot_us = 0;

void k85_log_init(void) {
    s_boot_us = esp_timer_get_time();
    s_count = 0;
    s_head = 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
void k85_log(const char *fmt, ...) {
    int64_t elapsed_s = (esp_timer_get_time() - s_boot_us) / 1000000;

    char msg[K85_LOG_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    char line[K85_LOG_LINE_LEN + 16];
    snprintf(line, sizeof(line), "[%llds] %s",
             (long long)elapsed_s, msg);
    // безопасно укладываем в фиксированный буфер строки лога
    snprintf(s_lines[s_head], K85_LOG_LINE_LEN, "%s", line);

    s_head = (s_head + 1) % K85_LOG_MAX;
    if (s_count < K85_LOG_MAX) s_count++;
}
#pragma GCC diagnostic pop

int k85_log_count(void) { return s_count; }

const char *k85_log_get(int index) {
    if (index < 0 || index >= s_count) return "";
    // самая старая запись находится по индексу s_head, если буфер полон;
    // если ещё не полон - самая старая находится в начале (0)
    int start = (s_count < K85_LOG_MAX) ? 0 : s_head;
    int real_idx = (start + index) % K85_LOG_MAX;
    return s_lines[real_idx];
}