#pragma once

// Кольцевой буфер последних уведомлений (в RAM, не персистится на диск)
void k85_notify(const char *fmt, ...);

// Кол-во непрочитанных — для бейджа в статус-баре
int k85_notifications_unread_count(void);

// Экран списка (последние сверху), помечает всё как прочитанное при заходе
void k85_run_notifications_screen(void);
