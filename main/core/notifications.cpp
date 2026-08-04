#include "notifications.h"
#include "text_input.h"

#include <cstring>
#include <cstdio>
#include <cstdarg>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define K85_NOTIF_MAX 8
#define K85_NOTIF_TEXT_LEN 40

struct K85Notification {
    char text[K85_NOTIF_TEXT_LEN];
    bool unread;
};

static K85Notification s_notifs[K85_NOTIF_MAX];
static int s_count = 0;
static int s_head = 0;
static SemaphoreHandle_t s_mutex = nullptr;

static void ensure_mutex(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

void k85_notify(const char *fmt, ...) {
    ensure_mutex();
    char buf[K85_NOTIF_TEXT_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        snprintf(s_notifs[s_head].text, K85_NOTIF_TEXT_LEN, "%s", buf);
        s_notifs[s_head].unread = true;
        s_head = (s_head + 1) % K85_NOTIF_MAX;
        if (s_count < K85_NOTIF_MAX) s_count++;
        xSemaphoreGive(s_mutex);
    }
}

int k85_notifications_unread_count(void) {
    if (!s_mutex) return 0;
    int n = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < s_count; i++) if (s_notifs[i].unread) n++;
        xSemaphoreGive(s_mutex);
    }
    return n;
}

void k85_run_notifications_screen(void) {
    ensure_mutex();
    static char lines_buf[K85_NOTIF_MAX][K85_NOTIF_TEXT_LEN];
    const char *lines[K85_NOTIF_MAX];
    int n = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        int idx = (s_head - 1 + K85_NOTIF_MAX) % K85_NOTIF_MAX;
        for (int i = 0; i < s_count; i++) {
            snprintf(lines_buf[n], K85_NOTIF_TEXT_LEN, "%s", s_notifs[idx].text);
            lines[n] = lines_buf[n];
            n++;
            s_notifs[idx].unread = false;
            idx = (idx - 1 + K85_NOTIF_MAX) % K85_NOTIF_MAX;
        }
        xSemaphoreGive(s_mutex);
    }

    if (n == 0) {
        static const char *empty[] = {"No notifications"};
        k85_area_show(empty, 1, "NOTIFICATIONS");
    } else {
        k85_area_show(lines, n, "NOTIFICATIONS");
    }
}
