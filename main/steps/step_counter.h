#pragma once

// Вызывать каждый цикл главного loop / lock screen (сам себя троттлит до 80мс)
void k85_step_counter_update(void);

int  k85_get_step_count(void);
void k85_reset_step_counter(void);