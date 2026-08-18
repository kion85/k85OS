#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "router_menu.h"
#include "ui/common.h"
#include "ui/list_menu.h"
#include "ui/text_input.h"
#include "core/input.h"
#include "core/config.h"
#include "core/log.h"
#include "net/router.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static char s_web_key[16];

static void gen_pass(char *out, int len, bool digits_only) {
    static const char cs_full[] = "ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    static const char cs_dig[]  = "0123456789";
    const char *cs = digits_only ? cs_dig : cs_full;
    int cs_len = digits_only ? (int)sizeof(cs_dig) - 1 : (int)sizeof(cs_full) - 1;
    for (int i = 0; i < len; i++) out[i] = cs[esp_random() % cs_len];
    out[len] = 0;
}

static void wait_ab_exit(void) {
    while (true) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); return; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ---------- Мини HTTP-хелперы (тот же паттерн, что в wifi_hotspot.cpp) ----------
static void url_decode(const char *src, char *dst, size_t dst_size) {
    size_t di = 0;
    while (*src && di + 1 < dst_size) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[di++] = (char)strtol(hex, nullptr, 16);
            src += 3;
        } else if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = 0;
}

static bool get_query_param(const char *query, const char *key, char *out, size_t out_size) {
    char search[40];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(query, search);
    if (!p) return false;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char raw[256];
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = 0;
    url_decode(raw, out, out_size);
    return true;
}

static bool check_key(const char *query) {
    char key[32] = {0};
    if (!get_query_param(query, "key", key, sizeof(key))) return false;
    return strcmp(key, s_web_key) == 0;
}

static void send_response(int fd, const char *status, const char *body) {
    char header[128];
    snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n", status);
    send(fd, header, strlen(header), 0);
    if (body) send(fd, body, strlen(body), 0);
}

static void send_redirect(int fd, const char *location) {
    char header[256];
    snprintf(header, sizeof(header), "HTTP/1.1 302 Found\r\nLocation: %s\r\nConnection: close\r\n\r\n", location);
    send(fd, header, strlen(header), 0);
}

static const char *login_html =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>k85OS Router</title>"
    "<style>body{background:#111;color:#0f0;font-family:monospace;padding:20px;max-width:400px;margin:auto}"
    "h1{color:#0ff}input,button{font-size:16px;margin-top:10px;padding:6px}"
    "input{background:#000;color:#0f0;border:1px solid #0ff;width:100%%}"
    "button{background:#0ff;color:#000;border:none;padding:8px 16px;cursor:pointer}"
    "</style></head><body><h1>k85OS Router Access</h1>"
    "<p>Enter the access code shown on the device screen.</p>"
    "<form method=GET action=/><input type=text name=key placeholder='access code' autofocus>"
    "<br><button type=submit>Enter</button></form></body></html>";

static void build_status_page(char *out, size_t out_size, const char *ssid, bool is_guest) {
    K85RouterLogEntry entries[K85_ROUTER_LOG_MAX];
    int n_log = k85_router_get_log(entries, K85_ROUTER_LOG_MAX);

    static char macs[K85_ROUTER_ACL_MAX][18];
    int n_acl = k85_router_acl_list(macs, K85_ROUTER_ACL_MAX);

    size_t used = 0;
    int w = snprintf(out, out_size,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>k85OS Router</title>"
        "<style>body{background:#111;color:#0f0;font-family:monospace;padding:16px;max-width:600px;margin:auto}"
        "h1{color:#0ff}.card{background:#222;padding:12px;margin:10px 0;border-radius:8px;border:1px solid #0ff}"
        "table{width:100%%;border-collapse:collapse;margin-top:10px}"
        "td,th{padding:6px;border-bottom:1px solid #333;text-align:left;font-size:14px}"
        "a{color:#0ff;text-decoration:none}"
        "a.del{color:#f66}"
        "input{background:#000;color:#0f0;border:1px solid #0ff;padding:4px;font-family:monospace}"
        "button{background:#0ff;color:#000;border:none;padding:6px 12px;font-family:monospace;cursor:pointer}"
        ".ok{color:#0f0}.warn{color:#ff0}"
        "</style></head><body>"
        "<h1>k85OS Router (%s)</h1>"
        "<div class='card'>"
        "<b>SSID:</b> %s<br>"
        "<b>Uplink (%s):</b> <span class='%s'>%s</span><br>"
        "<b>NAT / Internet sharing:</b> <span class='%s'>%s</span>"
        "</div>",
        is_guest ? "GUEST" : "HOME", ssid,
        g_config.wifi_ssid,
        k85_router_sta_connected() ? "ok" : "warn",
        k85_router_sta_connected() ? "connected" : "connecting...",
        k85_router_sta_connected() ? "ok" : "warn",
        k85_router_sta_connected() ? "active" : "waiting for uplink");
    if (w > 0) used = (size_t)w;

    int w2 = snprintf(out + used, out_size - used,
        "<h2>Access Control (ACL)</h2>"
        "<p>Empty list = all devices allowed. Add at least one MAC to switch to whitelist-only mode.</p>"
        "<form method=GET action=/acl/add>"
        "<input type=hidden name=key value='%s'>"
        "<input type=text name=mac placeholder='AA:BB:CC:DD:EE:FF' style='width:160px'>"
        "<button type=submit>+ Add</button></form>"
        "<table><tr><th>MAC</th><th></th></tr>", s_web_key);
    if (w2 > 0) used += w2;

    for (int i = 0; i < n_acl; i++) {
        char row[160];
        int wr = snprintf(row, sizeof(row),
            "<tr><td>%s</td><td><a class=del href='/acl/del?idx=%d&key=%s'>Remove</a></td></tr>",
            macs[i], i, s_web_key);
        if (wr > 0 && used + (size_t)wr < out_size - 500) { memcpy(out + used, row, wr); used += wr; }
    }

    int w3 = snprintf(out + used, out_size - used,
        "</table><h2>Connection Log</h2><table><tr><th>MAC</th><th>Event</th><th>t+sec</th></tr>");
    if (w3 > 0 && used + (size_t)w3 < out_size) used += w3;

    int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < n_log; i++) {
        char row[160];
        int wr = snprintf(row, sizeof(row),
            "<tr><td>%s</td><td>%s</td><td>%lld</td></tr>",
            entries[i].mac_str, entries[i].connected ? "connect" : "disconnect",
            (long long)((now_us - entries[i].timestamp_us) / 1000000));
        if (wr > 0 && used + (size_t)wr < out_size - 100) { memcpy(out + used, row, wr); used += wr; }
    }

        static char dmacs[K85_DNS_RULES_MAX][18];
    static char ddomains[K85_DNS_RULES_MAX][64];
    int n_dns = k85_router_dns_rule_list(dmacs, ddomains, K85_DNS_RULES_MAX);

    int w4 = snprintf(out + used, out_size - used,
        "</table><h2>DNS Rules (blocklist)</h2>"
        "<form method=GET action=/dns/add>"
        "<input type=hidden name=key value='%s'>"
        "<input type=text name=mac placeholder='* or AA:BB:CC:DD:EE:FF' style='width:150px'>"
        "<input type=text name=domain placeholder='ads.example.com' style='width:150px'>"
        "<button type=submit>+ Add</button></form>"
        "<table><tr><th>MAC</th><th>Domain</th><th></th></tr>", s_web_key);
    if (w4 > 0) used += w4;

    for (int i = 0; i < n_dns; i++) {
        char row[220];
        int wr = snprintf(row, sizeof(row),
            "<tr><td>%s</td><td>%s</td><td><a class=del href='/dns/del?idx=%d&key=%s'>Remove</a></td></tr>",
            dmacs[i], ddomains[i], i, s_web_key);
        if (wr > 0 && used + (size_t)wr < out_size - 300) { memcpy(out + used, row, wr); used += wr; }
    }

    const char *tail = "</table></body></html>";
    size_t tail_len = strlen(tail);
    if (used + tail_len < out_size) memcpy(out + used, tail, tail_len + 1);
}

static void handle_router_connection(int conn_fd, const char *ssid, bool is_guest) {
    static char req_buf[2048];
    long req_len = 0;
    long headers_end = -1;
    while (headers_end < 0 && req_len < (long)sizeof(req_buf) - 1) {
        int r = recv(conn_fd, req_buf + req_len, sizeof(req_buf) - 1 - req_len, 0);
        if (r <= 0) break;
        req_len += r;
        req_buf[req_len] = 0;
        for (long i = 0; i + 3 < req_len; i++) {
            if (!memcmp(req_buf + i, "\r\n\r\n", 4)) { headers_end = i; break; }
        }
    }
    if (headers_end < 0) return;

    char method[8] = {0}, path_full[256] = {0};
    sscanf(req_buf, "%7s %255s", method, path_full);

    char path[128] = {0}, query[256] = {0};
    char *qmark = strchr(path_full, '?');
    if (qmark) {
        size_t plen = (size_t)(qmark - path_full);
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, path_full, plen); path[plen] = 0;
        snprintf(query, sizeof(query), "%s", qmark + 1);
    } else {
        snprintf(path, sizeof(path), "%s", path_full);
    }

    if (!check_key(query)) {
        send_response(conn_fd, "401 Unauthorized", login_html);
        return;
    }

    if (strcmp(path, "/dns/add") == 0) {
        char mac[18] = "*", domain[64] = {0};
        get_query_param(query, "mac", mac, sizeof(mac));
        get_query_param(query, "domain", domain, sizeof(domain));
        if (strlen(mac) > 0 && strlen(domain) > 0) k85_router_dns_rule_add(mac, domain);
        char redir[64];
        snprintf(redir, sizeof(redir), "/?key=%s", s_web_key);
        send_redirect(conn_fd, redir);
    } else if (strcmp(path, "/dns/del") == 0) {
        char idx_str[8] = {0};
        get_query_param(query, "idx", idx_str, sizeof(idx_str));
        k85_router_dns_rule_remove(atoi(idx_str));
        char redir[64];
        snprintf(redir, sizeof(redir), "/?key=%s", s_web_key);
        send_redirect(conn_fd, redir);
    } else if (strcmp(path, "/acl/add") == 0) {
        char mac[18] = {0};
        get_query_param(query, "mac", mac, sizeof(mac));
        if (strlen(mac) == 17) k85_router_acl_add(mac);
        char redir[64];
        snprintf(redir, sizeof(redir), "/?key=%s", s_web_key);
        send_redirect(conn_fd, redir);
    } else if (strcmp(path, "/acl/del") == 0) {
        char idx_str[8] = {0};
        get_query_param(query, "idx", idx_str, sizeof(idx_str));
        k85_router_acl_remove(atoi(idx_str));
        char redir[64];
        snprintf(redir, sizeof(redir), "/?key=%s", s_web_key);
        send_redirect(conn_fd, redir);
    } else {
        static char body[6144];
        build_status_page(body, sizeof(body), ssid, is_guest);
        send_response(conn_fd, "200 OK", body);
    }
}

static void run_router_start(bool is_guest) {
    if (!g_config.wifi_saved || g_config.wifi_ssid[0] == '\0') {
        k85_show_message("No saved uplink WiFi\nConnect via WiFi Manager first\nA+B=back");
        wait_ab_exit();
        return;
    }

    char ssid[32];
    snprintf(ssid, sizeof(ssid), is_guest ? "k85OS-Guest-%04X" : "k85OS-Home-%04X",
             (unsigned)(esp_random() & 0xFFFF));
    char ap_password[16];
    gen_pass(ap_password, 10, false);

    if (!k85_router_start(ssid, ap_password, false, is_guest)) {
        k85_show_message("Router start failed\nA+B=back");
        wait_ab_exit();
        return;
    }

    gen_pass(s_web_key, 6 + (int)(esp_random() % 5), false);

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        k85_show_message("Socket error");
        k85_router_stop();
        return;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(80);
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 2);

    uint32_t last_redraw = 0;
    bool running = true;
    while (running) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            running = false;
            break;
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_redraw > 500) {
            char msg[220];
            snprintf(msg, sizeof(msg),
                     "%s Hotspot\nSSID: %s\nPass: %s\nUplink: %s\nWeb: http://192.168.4.1\nKey: %s\nA+B=stop",
                     is_guest ? "GUEST" : "HOME", ssid, ap_password,
                     k85_router_sta_connected() ? "connected" : "connecting...",
                     s_web_key);
            k85_show_message(msg);
            last_redraw = now;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        struct timeval tv = {0, 100000};
        int sel = select(listen_fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(listen_fd, &fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (conn_fd >= 0) {
                struct timeval sock_timeout = { .tv_sec = 3, .tv_usec = 0 };
                setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &sock_timeout, sizeof(sock_timeout));
                setsockopt(conn_fd, SOL_SOCKET, SO_SNDTIMEO, &sock_timeout, sizeof(sock_timeout));
                handle_router_connection(conn_fd, ssid, is_guest);
                close(conn_fd);
            }
        }
    }
    close(listen_fd);
    k85_router_stop();
}

static void run_router_log(void) {
    K85RouterLogEntry entries[K85_ROUTER_LOG_MAX];
    int n = k85_router_get_log(entries, K85_ROUTER_LOG_MAX);
    if (n == 0) {
        k85_show_message("No connection events\nA+B=back");
        wait_ab_exit();
        return;
    }
    static char labels[K85_ROUTER_LOG_MAX][48];
    static const char *names[K85_ROUTER_LOG_MAX];
    for (int i = 0; i < n; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%s %s",
                 entries[i].connected ? "+" : "-", entries[i].mac_str);
        names[i] = labels[i];
    }
    k85_run_list_menu("CONNECTION LOG", names, n, nullptr);
}

static void run_router_acl(void) {
    while (true) {
        static char macs[K85_ROUTER_ACL_MAX][18];
        int n = k85_router_acl_list(macs, K85_ROUTER_ACL_MAX);

        static const char *items[K85_ROUTER_ACL_MAX + 2];
        static char labels[K85_ROUTER_ACL_MAX][24];
        for (int i = 0; i < n; i++) {
            snprintf(labels[i], sizeof(labels[i]), "%s", macs[i]);
            items[i] = labels[i];
        }
        items[n] = "+ Add MAC";
        items[n + 1] = "Back";

        int idx = k85_run_list_menu("ACL (empty=allow all)", items, n + 2, nullptr);
        if (idx < 0 || idx == n + 1) return;

        if (idx == n) {
            char mac[18] = "";
            if (k85_text_input("MAC (AA:BB:CC:DD:EE:FF):", "", mac, sizeof(mac)) && strlen(mac) == 17) {
                k85_router_acl_add(mac);
            }
        } else {
            k85_router_acl_remove(idx);
        }
    }
}

static void run_router_dns_rules(void) {
    while (true) {
        static char macs[K85_DNS_RULES_MAX][18];
        static char domains[K85_DNS_RULES_MAX][64];
        int n = k85_router_dns_rule_list(macs, domains, K85_DNS_RULES_MAX);

        static const char *items[K85_DNS_RULES_MAX + 2];
        static char labels[K85_DNS_RULES_MAX][90];
        for (int i = 0; i < n; i++) {
            snprintf(labels[i], sizeof(labels[i]), "%s -> %s", macs[i], domains[i]);
            items[i] = labels[i];
        }
        items[n] = "+ Add rule";
        items[n + 1] = "Back";

        int idx = k85_run_list_menu("DNS RULES", items, n + 2, nullptr);
        if (idx < 0 || idx == n + 1) return;

        if (idx == n) {
            char mac[18] = "";
            if (!k85_text_input("MAC (or * for all):", "*", mac, sizeof(mac))) continue;
            if (strlen(mac) == 0) continue;

            char domain[64] = "";
            if (!k85_text_input("Domain (e.g. ads.example.com):", "", domain, sizeof(domain))) continue;
            if (strlen(domain) == 0) continue;

            k85_router_dns_rule_add(mac, domain);
        } else {
            k85_router_dns_rule_remove(idx);
        }
    }
}

void k85_run_router_menu(void) {
    static const char *items[] = {"Start Home Hotspot", "Start Guest Hotspot", "Connection Log", "Access Control (ACL)", "DNS Rules", "Back"};
    while (true) {
        int idx = k85_run_list_menu("NETWORK / ROUTER", items, 6, nullptr);
        if (idx < 0 || idx == 5) break;
        if (idx == 0) run_router_start(false);
        else if (idx == 1) run_router_start(true);
        else if (idx == 2) run_router_log();
        else if (idx == 3) run_router_acl();
        else if (idx == 4) run_router_dns_rules();
    }
}

#pragma GCC diagnostic pop


