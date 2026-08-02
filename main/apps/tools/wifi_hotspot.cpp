#include "wifi_hotspot.h"
#include "wifi.h"
#include "common.h"
#include "power.h"
#include "input.h"
#include "log.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// Должен совпадать с K85_LITTLEFS_BASE_PATH из core/config, если такая константа
// уже есть в проекте - лучше подключить её оттуда вместо дублирования здесь.
#define K85_UPLOAD_BASE_PATH "/littlefs"

static esp_netif_t *s_ap_netif = nullptr;

static void build_status_page(char *out, size_t out_size, const char *ssid) {
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    snprintf(out, out_size,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>k85OS</title>"
        "<style>body{background:#111;color:#0f0;font-family:monospace;padding:20px}"
        "h1{color:#0ff}.card{background:#222;padding:15px;margin:10px 0;border-radius:8px;border:1px solid #0ff}"
        ".val{font-size:24px;color:#fff}a{color:#0ff}</style></head><body>"
        "<h1>k85OS</h1>"
        "<div class='card'>"
        "<b>AP SSID:</b> <span class='val'>%s</span><br>"
        "<b>Uptime:</b> <span class='val'>%llds</span><br>"
        "<b>Free heap:</b> <span class='val'>%lu bytes</span><br>"
        "</div>"
        "<p><a href='/upload'>Upload file</a></p>"
        "</body></html>",
        ssid, (long long)uptime_s, (unsigned long)esp_get_free_heap_size());
}

static const char *k85_upload_form_html =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>k85OS Upload</title>"
    "<style>body{background:#111;color:#0f0;font-family:monospace;padding:20px}"
    "h1{color:#0ff}input,button{font-size:16px;margin-top:10px}"
    "a{color:#0ff}</style></head><body>"
    "<h1>Upload to /littlefs</h1>"
    "<form method=POST action=/upload enctype=multipart/form-data>"
    "<input type=file name=file><br>"
    "<button type=submit>Upload</button>"
    "</form>"
    "<p><a href=/>Back</a></p></body></html>";

static const char *k85_upload_ok_html =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>"
    "<body style='background:#111;color:#0f0;font-family:monospace;padding:20px'>"
    "<h1 style='color:#0ff'>OK</h1><p>File saved.</p>"
    "<p><a href=/upload style='color:#0ff'>Upload another</a></p></body></html>";

static const char *k85_upload_fail_html =
    "<!DOCTYPE html><html><body style='background:#111;color:#f66;font-family:monospace;padding:20px'>"
    "<h1>Upload failed</h1><p><a href=/upload style='color:#0ff'>Try again</a></p></body></html>";

static void send_http_response(int fd, const char *status, const char *body) {
    char header[128];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n", status);
    send(fd, header, strlen(header), 0);
    send(fd, body, strlen(body), 0);
}

// Ищет needle в буфере [buf, buf+len). Возвращает смещение или -1, если не нашли.
static long find_bytes(const char *buf, long len, const char *needle, long needle_len) {
    if (needle_len <= 0 || len < needle_len) return -1;
    for (long i = 0; i <= len - needle_len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) return i;
    }
    return -1;
}

// Достаёт значение вида key="value" из строки заголовков части multipart.
static bool extract_quoted(const char *hay, const char *key, char *out, size_t out_size) {
    const char *p = strstr(hay, key);
    if (!p) return false;
    p += strlen(key);
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

// Оставляет только базовое имя файла, без пути и без пустой строки.
static void sanitize_filename(char *name) {
    char *slash = strrchr(name, '/');
    char *bslash = strrchr(name, '\\');
    char *base = name;
    if (slash && slash + 1 > base) base = slash + 1;
    if (bslash && bslash + 1 > base) base = bslash + 1;
    if (base != name) memmove(name, base, strlen(base) + 1);
    if (name[0] == 0) {
        strncpy(name, "upload.bin", 32);
    }
}

// Разбирает multipart/form-data и стримит содержимое файла сразу на диск.
// header           - полностью прочитанные заголовки HTTP-запроса (null-terminated).
// initial_body     - то, что уже успели прочитать из тела вместе с заголовками.
// initial_body_len - длина initial_body в байтах.
static bool handle_upload_body(int conn_fd, const char *header,
                                const char *initial_body, long initial_body_len) {
    const char *b = strstr(header, "boundary=");
    if (!b) return false;
    b += strlen("boundary=");

    char delim[132];
    size_t di = 0;
    delim[di++] = '-';
    delim[di++] = '-';
    while (*b && *b != '\r' && *b != '\n' && *b != ';' && di < sizeof(delim) - 1) {
        delim[di++] = *b++;
    }
    delim[di] = 0;
    long delim_len = (long)strlen(delim);
    if (delim_len <= 2) return false;

    // static, чтобы не сажать 4KB на стек задачи меню/хотспота
    static char buf[4096];
    long have = 0;
    if (initial_body_len > 0) {
        long n = initial_body_len;
        if (n > (long)sizeof(buf)) n = sizeof(buf);
        memcpy(buf, initial_body, n);
        have = n;
    }

    // Дочитываем, пока не найдём конец заголовков самой части (Content-Disposition и т.п.)
    long part_headers_end = find_bytes(buf, have, "\r\n\r\n", 4);
    while (part_headers_end < 0) {
        if (have >= (long)sizeof(buf) - 1) return false;
        int r = recv(conn_fd, buf + have, sizeof(buf) - have, 0);
        if (r <= 0) return false;
        have += r;
        part_headers_end = find_bytes(buf, have, "\r\n\r\n", 4);
    }

    char part_headers[512];
    long ph_len = part_headers_end < (long)sizeof(part_headers) - 1
                      ? part_headers_end : (long)sizeof(part_headers) - 1;
    memcpy(part_headers, buf, ph_len);
    part_headers[ph_len] = 0;

    char filename[128] = {0};
    if (!extract_quoted(part_headers, "filename=\"", filename, sizeof(filename))) return false;
    sanitize_filename(filename);
    if (filename[0] == 0) return false;

    char full_path[192];
    snprintf(full_path, sizeof(full_path), "%s/%s", K85_UPLOAD_BASE_PATH, filename);

    FILE *f = fopen(full_path, "wb");
    if (!f) {
        k85_log("upload: fopen failed for %s", full_path);
        return false;
    }

    long data_start = part_headers_end + 4;
    long pending_len = have - data_start;
    if (pending_len > 0) memmove(buf, buf + data_start, pending_len);
    have = pending_len;

    bool ok = false;
    while (true) {
        long dpos = find_bytes(buf, have, delim, delim_len);
        if (dpos >= 0) {
            long write_len = dpos;
            if (write_len >= 2 && buf[write_len - 2] == '\r' && buf[write_len - 1] == '\n') {
                write_len -= 2;
            }
            if (write_len > 0) fwrite(buf, 1, write_len, f);
            ok = true;
            break;
        }

        // Ничего похожего на границу не найдено - можно смело сбросить на диск всё,
        // кроме "хвоста" длиной delim_len+2 (граница может разрезаться между recv'ами).
        long safe_len = have - (delim_len + 2);
        if (safe_len > 0) {
            fwrite(buf, 1, safe_len, f);
            memmove(buf, buf + safe_len, have - safe_len);
            have -= safe_len;
        }
        if (have >= (long)sizeof(buf)) break; // не должно случиться при разумном boundary

        int r = recv(conn_fd, buf + have, sizeof(buf) - have, 0);
        if (r <= 0) break; // соединение оборвалось раньше времени
        have += r;
    }

    fclose(f);
    if (!ok) {
        remove(full_path);
        k85_log("upload: incomplete, removed %s", full_path);
        return false;
    }
    k85_log("upload: saved %s", full_path);
    return true;
}

static void handle_connection(int conn_fd, const char *ssid) {
    static char req_buf[4096];
    long req_len = 0;
    long headers_end = -1;

    while (headers_end < 0 && req_len < (long)sizeof(req_buf) - 1) {
        int r = recv(conn_fd, req_buf + req_len, sizeof(req_buf) - 1 - req_len, 0);
        if (r <= 0) break;
        req_len += r;
        req_buf[req_len] = 0;
        headers_end = find_bytes(req_buf, req_len, "\r\n\r\n", 4);
    }

    if (headers_end < 0) return; // не дождались полных заголовков - молча закрываем

    bool is_post_upload = (strncmp(req_buf, "POST /upload", 12) == 0);
    bool is_get_upload  = (strncmp(req_buf, "GET /upload", 11) == 0);

    if (is_post_upload) {
        long body_have = req_len - (headers_end + 4);
        bool ok = handle_upload_body(conn_fd, req_buf, req_buf + headers_end + 4, body_have);
        send_http_response(conn_fd, ok ? "200 OK" : "400 Bad Request",
                            ok ? k85_upload_ok_html : k85_upload_fail_html);
    } else if (is_get_upload) {
        send_http_response(conn_fd, "200 OK", k85_upload_form_html);
    } else {
        char body[1024];
        build_status_page(body, sizeof(body), ssid);
        send_http_response(conn_fd, "200 OK", body);
    }
}

void k85_run_wifi_hotspot(void) {
    // esp_netif_init()/esp_event_loop_create_default()/esp_wifi_init() должны быть
    // выполнены до esp_netif_create_default_wifi_ap() - иначе esp_wifi_set_default_wifi_ap_handlers()
    // падает с ESP_ERR_INVALID_STATE, если пользователь зашёл в Hotspot,
    // ни разу не подключаясь к WiFi через k85_wifi_connect*(). Функция идемпотентна.
    k85_wifi_init();

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "k85OS-%02X%02X", mac[4], mac[5]);
    const char *password = "12345678";

    wifi_config_t ap_config = {};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", ssid);
    ap_config.ap.ssid_len = strlen(ssid);
    snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s", password);
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection = 4;
    ap_config.ap.channel = 1;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_STATE) {
        k85_log("Hotspot start failed: %d", (int)start_err);
        k85_show_message("Hotspot failed\nA+B=back");
        return;
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_ap_netif, &ip_info);
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

    k85_log("Hotspot started: %s @ %s", ssid, ip_str);
    char msg[80];
    snprintf(msg, sizeof(msg), "AP: %s\nIP: %s\nA+B=stop", ssid, ip_str);
    k85_show_message(msg);

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        k85_show_message("Socket error");
        esp_wifi_set_mode(WIFI_MODE_STA);
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

    bool running = true;
    while (running) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            running = false;
            break;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        struct timeval tv = {0, 200000}; // 200ms
        int sel = select(listen_fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(listen_fd, &fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (conn_fd >= 0) {
                handle_connection(conn_fd, ssid);
                close(conn_fd);
            }
        }
    }
    close(listen_fd);

    esp_wifi_set_mode(WIFI_MODE_STA);
    k85_log("Hotspot stopped");
    k85_show_message("Hotspot stopped");
    vTaskDelay(pdMS_TO_TICKS(1000));
}

