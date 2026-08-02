#pragma once
#include <stdbool.h>

void k85_rtc_ntp_init(void);          // выставляет TZ (MSK-3), вызывать раз при старте
bool k85_ntp_sync_now(void);          // блокирующая попытка синхронизации (5с таймаут)
bool k85_is_ntp_synced(void);

// Аналоги get_time_str()/get_date_str()/get_uptime_str() из MicroPython.
// Возвращают указатель на внутренний статический буфер (валиден до следующего вызова).
const char *k85_get_time_str(void);
const char *k85_get_date_str(void);
const char *k85_get_uptime_str(void);