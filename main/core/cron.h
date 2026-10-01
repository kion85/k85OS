#pragma once

#include <stdbool.h>
#include <stddef.h>

// Cron-like scheduler. Jobs live in /littlefs/crontab, one per line:
//   min hour day-of-month month day-of-week command
// Fields: *  */N  a  a-b  a-b/N  a,b,c   (day-of-week 0-7, 0 and 7 = Sunday)
// Lines starting with # are ignored. Time = NTP time + utc_offset from config.
// Jobs do not run until the clock has been set by NTP.
// Output of each run is appended to /littlefs/cron.log (auto-trimmed).

bool k85_cron_start(void);          // loads crontab and starts the task
void k85_cron_stop(void);           // asks the task to exit (within ~5 s)
bool k85_cron_is_running(void);
int  k85_cron_reload(void);         // re-reads crontab, returns job count
int  k85_cron_job_count(void);
int  k85_cron_list(char *out, size_t out_size); // formatted list, returns job count
