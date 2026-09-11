#include "ping.h"
#include "common.h"
#include "theme.h"
#include "input.h"
#include "text_input.h"
#include "log.h"

#include "ping/ping_sock.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

// Поля volatile, т.к. обновляются из колбэков esp_ping (другой контекст
// выполнения), а читаются из основного UI-цикла ниже.
struct PingStats {
    volatile int sent;
    volatile int received;
    volatile uint32_t last_rtt_ms;
    volatile uint32_t min_rtt_ms;
    volatile uint32_t max_rtt_ms;
    volatile uint64_t total_rtt_ms;
    char target_ip[24];
};

static PingStats s_stats;

static void reset_stats(void) {
    s_stats.sent = 0;
    s_stats.received = 0;
    s_stats.last_rtt_ms = 0;
    s_stats.min_rtt_ms = 0xFFFFFFFF;
    s_stats.max_rtt_ms = 0;
    s_stats.total_rtt_ms = 0;
    s_stats.target_ip[0] = 0;
}

static void on_ping_success(esp_ping_handle_t hdl, void *args) {
    uint32_t elapsed_time = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    s_stats.sent++;
    s_stats.received++;
    s_stats.last_rtt_ms = elapsed_time;
    if (elapsed_time < s_stats.min_rtt_ms) s_stats.min_rtt_ms = elapsed_time;
    if (elapsed_time > s_stats.max_rtt_ms) s_stats.max_rtt_ms = elapsed_time;
    s_stats.total_rtt_ms += elapsed_time;
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    s_stats.sent++;
    s_stats.last_rtt_ms = 0; // 0 используется в UI как признак "timeout"
}

static void on_ping_end(esp_ping_handle_t hdl, void *args) {
    // count=0 (бесконечный пинг) практически никогда сюда не попадает -
    // остановка происходит через esp_ping_stop() при выходе пользователя.
}

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void k85_run_ping(void) {
    char host[64] = "";
    if (!k85_text_input("Ping host/IP:", "", host, sizeof(host)) || !host[0]) return;

    // Обычный DNS-резолвинг (интернет-хосты и "голые" IP). Имена вида
    // *.local (mDNS) этим путём НЕ резолвятся - для них нужен отдельный
    // mdns_query_A(), это не входит в этот инструмент.
    struct addrinfo hint = {};
    struct addrinfo *res = nullptr;
    hint.ai_family = AF_INET;
    int err = getaddrinfo(host, nullptr, &hint, &res);
    if (err != 0 || !res) {
        k85_show_message("DNS resolve failed\nA+B=back");
        wait_ab_exit();
        return;
    }

    struct sockaddr_in addr_copy;
    memcpy(&addr_copy, res->ai_addr, sizeof(addr_copy));
    freeaddrinfo(res);

    ip_addr_t target_addr;
    ip_addr_set_ip4_u32(&target_addr, addr_copy.sin_addr.s_addr);

    reset_stats();
    snprintf(s_stats.target_ip, sizeof(s_stats.target_ip), "%s", inet_ntoa(addr_copy.sin_addr));

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target_addr;
    config.count = 0; // 0 = непрерывно, пока не остановим сами (A+B)
    config.interval_ms = 1000;
    config.timeout_ms = 1000;

    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = on_ping_success;
    cbs.on_ping_timeout = on_ping_timeout;
    cbs.on_ping_end = on_ping_end;

    esp_ping_handle_t ping = nullptr;
    if (esp_ping_new_session(&config, &cbs, &ping) != ESP_OK) {
        k85_show_message("Ping session failed\nA+B=back");
        wait_ab_exit();
        return;
    }
    esp_ping_start(ping);

    uint32_t bg = k85_get_bg();
    uint32_t fg = k85_get_fg();
    uint32_t accent = k85_get_accent();

    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            break;
        }

        M5.Display.fillScreen(bg);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(accent, bg);
        M5.Display.setCursor(4, 4);
        M5.Display.printf("Ping %s", s_stats.target_ip);

        char line[48];
        int y = 20;

        snprintf(line, sizeof(line), "Sent: %d  Recv: %d", s_stats.sent, s_stats.received);
        M5.Display.setTextColor(fg, bg);
        M5.Display.setCursor(4, y); M5.Display.print(line); y += 14;

        int loss_pct = s_stats.sent > 0 ? (100 * (s_stats.sent - s_stats.received) / s_stats.sent) : 0;
        snprintf(line, sizeof(line), "Loss: %d%%", loss_pct);
        M5.Display.setCursor(4, y); M5.Display.print(line); y += 14;

        if (s_stats.sent == 0) {
            snprintf(line, sizeof(line), "Last: -");
        } else if (s_stats.last_rtt_ms > 0) {
            snprintf(line, sizeof(line), "Last: %u ms", (unsigned)s_stats.last_rtt_ms);
        } else {
            snprintf(line, sizeof(line), "Last: timeout");
        }
        M5.Display.setCursor(4, y); M5.Display.print(line); y += 14;

        if (s_stats.received > 0) {
            uint32_t avg = (uint32_t)(s_stats.total_rtt_ms / (uint64_t)s_stats.received);
            snprintf(line, sizeof(line), "Min/Avg/Max: %u/%u/%u ms",
                     (unsigned)s_stats.min_rtt_ms, (unsigned)avg, (unsigned)s_stats.max_rtt_ms);
            M5.Display.setCursor(4, y); M5.Display.print(line); y += 14;
        }

        M5.Display.setTextColor(0xAAAAAA, bg);
        M5.Display.setCursor(4, M5.Display.height() - 12);
        M5.Display.print("A+B=stop");

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
}