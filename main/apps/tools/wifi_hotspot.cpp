#include "wifi_hotspot.h"
#include "wifi.h"
#include "common.h"
#include "power.h"
#include "input.h"
#include "log.h"
#include "list_menu.h"
#include "text_input.h"
#include "../../core/config.h"
#include "../../net/firmware_flash.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define K85_UPLOAD_BASE_PATH "/littlefs"
#define K85_FM_MAX_ENTRIES 40
#define K85_MAX_UPLOAD_BYTES (2 * 1024 * 1024)

static esp_netif_t *s_ap_netif = nullptr;
static char s_web_access_key[16];
static int s_failed_attempts = 0;
static int64_t s_lockout_until_us = 0;
static int64_t s_conn_deadline_us = 0;

#define K85_HOTSPOT_TASK_STACK_BYTES 8192
static_assert(sizeof(StackType_t) == 1,
    "StackType_t не uint8_t - пересчитай K85_HOTSPOT_TASK_STACK_BYTES!");
static StaticTask_t s_hotspot_task_buf;
static StackType_t *s_hotspot_task_stack = nullptr;
static TaskHandle_t s_hotspot_task_handle = nullptr;
static volatile bool s_hotspot_task_running = false;
static int s_hotspot_listen_fd = -1;
static char s_hotspot_ssid[32] = "";

static bool const_time_streq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    volatile unsigned char diff = (unsigned char)(la != lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

static void gen_random_string(char *out, int len, bool digits_only) {
    static const char charset_full[] = "ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    static const char charset_digits[] = "0123456789";
    const char *cs = digits_only ? charset_digits : charset_full;
    int cs_len = digits_only ? (int)sizeof(charset_digits) - 1 : (int)sizeof(charset_full) - 1;
    for (int i = 0; i < len; i++) out[i] = cs[esp_random() % cs_len];
    out[len] = 0;
}

static void url_decode(const char *src, char *dst, size_t dst_size) {
    size_t di = 0;
    while (*src && di + 1 < dst_size) {
        char decoded;
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            decoded = (char)strtol(hex, nullptr, 16);
            src += 3;
        } else if (*src == '+') {
            decoded = ' ';
            src++;
        } else {
            decoded = *src++;
        }
        if (decoded == '\r' || decoded == '\n') continue;
        dst[di++] = decoded;
    }
    dst[di] = 0;
}
static bool sanitize_relpath(const char *name) {
    if (!name[0]) return false;
    if (strstr(name, "..")) return false;
    if (name[0] == '/') return false;
    for (const char *p = name; *p; p++) {
        if ((unsigned char)*p < 0x20) return false;
    }
    return true;
}

// Тот же чёрный список, что и в SSH-shell (shell_commands.cpp) - SSH-ключ
// и файл конфига (WiFi-пароли, хеши) не должны отдаваться через веб-морду
// ни при каких обстоятельствах, независимо от того, знает ли клиент access key.
static bool is_blocked_download_path(const char *path) {
    static const char *blocked[] = { "k85_ssh_host_key", "k85os_config.json", "k85_ssh_" };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        if (strstr(path, blocked[i])) return true;
    }
    return false;
}

static void send_http_response(int fd, const char *status, const char *content_type, const char *body) {
    char header[160];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", status, content_type);
    send(fd, header, strlen(header), 0);
    if (body) send(fd, body, strlen(body), 0);
}

static void send_http_redirect(int fd, const char *location) {
    char header[256];
    snprintf(header, sizeof(header),
             "HTTP/1.1 302 Found\r\nLocation: %s\r\nConnection: close\r\n\r\n", location);
    send(fd, header, strlen(header), 0);
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

// Ищет "Cookie: ... k85key=VALUE ..." в блоке сырых заголовков запроса.
static bool get_cookie_value(const char *headers_block, size_t headers_len, const char *name, char *out, size_t out_size) {
    const char *cookie_hdr = nullptr;
    for (size_t i = 0; i + 7 < headers_len; i++) {
        if (strncasecmp(headers_block + i, "Cookie:", 7) == 0) { cookie_hdr = headers_block + i + 7; break; }
    }
    if (!cookie_hdr) return false;

    char name_eq[40];
    snprintf(name_eq, sizeof(name_eq), "%s=", name);
    const char *p = strstr(cookie_hdr, name_eq);
    if (!p) return false;
    p += strlen(name_eq);
    size_t len = 0;
    while (p[len] && p[len] != ';' && p[len] != '\r' && p[len] != '\n') len++;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

// Проверяет доступ: сначала по cookie (обычная навигация после логина -
// ключ никогда не виден в адресной строке, все внутренние ссылки его
// больше не содержат вообще), с фоллбэком на query-параметр ?key= только
// для случая, если кто-то вручную ввёл/сохранил старую ссылку.
static bool check_access_key(const char *headers_block, size_t headers_len, const char *query) {
    int64_t now = esp_timer_get_time();
    if (now < s_lockout_until_us) return false;

    char cookie_key[32] = {0};
    if (get_cookie_value(headers_block, headers_len, "k85key", cookie_key, sizeof(cookie_key))) {
        if (const_time_streq(cookie_key, s_web_access_key)) {
            s_failed_attempts = 0;
            return true;
        }
    }

    char key[32] = {0};
    if (!get_query_param(query, "key", key, sizeof(key))) return false;

    bool ok = const_time_streq(key, s_web_access_key);
    if (!ok) {
        s_failed_attempts++;
        int shift = s_failed_attempts > 5 ? 5 : s_failed_attempts - 1;
        int delay_s = 1 << shift;
        if (delay_s > 30) delay_s = 30;
        s_lockout_until_us = now + (int64_t)delay_s * 1000000;
        k85_log("web-fm: bad access key attempt #%d, locked %ds", s_failed_attempts, delay_s);
    } else {
        s_failed_attempts = 0;
    }
    return ok;
}

static const char *k85_login_page_html =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>k85OS Login</title>"
    "<style>body{background:#111;color:#0f0;font-family:monospace;padding:20px;max-width:400px;margin:auto}"
    "h1{color:#0ff}input,button{font-size:16px;margin-top:10px;padding:6px}"
    "input{background:#000;color:#0f0;border:1px solid #0ff;width:100%%}"
    "button{background:#0ff;color:#000;border:none;padding:8px 16px;cursor:pointer}"
    "</style></head><body>"
    "<h1>k85OS Access</h1>"
    "<p>Enter the access code shown on the device screen.</p>"
    "<form method=POST action=/login>"
    "<input type=text name=key placeholder='access code' autofocus>"
    "<br><button type=submit>Enter</button>"
    "</form></body></html>";

static const char *k85_upload_ok_html =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>"
    "<body style='background:#111;color:#0f0;font-family:monospace;padding:20px'>"
    "<h1 style='color:#0ff'>OK</h1><p>File saved.</p>"
    "<p><a href=/ style='color:#0ff'>Back</a></p></body></html>";

static const char *k85_upload_fail_html =
    "<!DOCTYPE html><html><body style='background:#111;color:#f66;font-family:monospace;padding:20px'>"
    "<h1>Upload failed</h1><p><a href=/ style='color:#0ff'>Back</a></p></body></html>";

static const char *k85_generic_fail_html =
    "<!DOCTYPE html><html><body style='background:#111;color:#f66;font-family:monospace;padding:20px'>"
    "<h1>Error</h1><p><a href=/ style='color:#0ff'>Back</a></p></body></html>";

static long find_bytes(const char *buf, long len, const char *needle, long needle_len) {
    if (needle_len <= 0 || len < needle_len) return -1;
    for (long i = 0; i <= len - needle_len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) return i;
    }
    return -1;
}

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

static void sanitize_filename(char *name) {
    char *slash = strrchr(name, '/');
    char *bslash = strrchr(name, '\\');
    char *base = name;
    if (slash && slash + 1 > base) base = slash + 1;
    if (bslash && bslash + 1 > base) base = bslash + 1;
    if (base != name) memmove(name, base, strlen(base) + 1);

    char *w = name;
    for (char *r = name; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c < 0x20) continue;
        if (c == '<' || c == '>' || c == '&' || c == '"' || c == 39) continue;
        *w++ = (char)c;
    }
    *w = 0;

    if (name[0] == 0) strncpy(name, "upload.bin", 32);
}

static bool handle_upload_body(int conn_fd, const char *header,
                                const char *initial_body, long initial_body_len,
                                const char *target_dir) {
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

    static char buf[4096];
    long have = 0;
    if (initial_body_len > 0) {
        long n = initial_body_len;
        if (n > (long)sizeof(buf)) n = sizeof(buf);
        memcpy(buf, initial_body, n);
        have = n;
    }

    long part_headers_end = find_bytes(buf, have, "\r\n\r\n", 4);
    while (part_headers_end < 0) {
        if (esp_timer_get_time() > s_conn_deadline_us) return false;
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

    char full_path[256];
    if (target_dir && target_dir[0]) {
        snprintf(full_path, sizeof(full_path), "%s/%s/%s", K85_UPLOAD_BASE_PATH, target_dir, filename);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", K85_UPLOAD_BASE_PATH, filename);
    }

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
    long total_written = 0;
    while (true) {
        long dpos = find_bytes(buf, have, delim, delim_len);
        if (dpos >= 0) {
            long write_len = dpos;
            if (write_len >= 2 && buf[write_len - 2] == '\r' && buf[write_len - 1] == '\n') {
                write_len -= 2;
            }
            if (write_len > 0) fwrite(buf, 1, write_len, f);
            total_written += write_len;
            ok = true;
            break;
        }
        long safe_len = have - (delim_len + 2);
        if (safe_len > 0) {
            fwrite(buf, 1, safe_len, f);
            total_written += safe_len;
            memmove(buf, buf + safe_len, have - safe_len);
            have -= safe_len;
        }
        if (total_written > K85_MAX_UPLOAD_BYTES) { k85_log("upload: exceeded max size, aborting"); ok = false; break; }
        if (esp_timer_get_time() > s_conn_deadline_us) break;
        if (have >= (long)sizeof(buf)) break;
        int r = recv(conn_fd, buf + have, sizeof(buf) - have, 0);
        if (r <= 0) break;
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

// Форма теперь содержит ДВА поля: file (бинарник) и sig (hex-подпись).
// Браузеры отправляют части в порядке полей формы - у нас file идёт первым,
// sig вторым. Сначала стримим file до первой границы (как раньше), затем
// доразбираем оставшийся буфер (и при нужде дочитываем ещё) чтобы найти
// значение поля sig между его заголовками и следующей границей.
static bool handle_firmware_upload_body(int conn_fd, const char *header,
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

    static char buf[4096];
    long have = 0;
    if (initial_body_len > 0) {
        long n = initial_body_len;
        if (n > (long)sizeof(buf)) n = sizeof(buf);
        memcpy(buf, initial_body, n);
        have = n;
    }

    long part_headers_end = find_bytes(buf, have, "\r\n\r\n", 4);
    while (part_headers_end < 0) {
        if (esp_timer_get_time() > s_conn_deadline_us) return false;
        if (have >= (long)sizeof(buf) - 1) return false;
        int r = recv(conn_fd, buf + have, sizeof(buf) - have, 0);
        if (r <= 0) return false;
        have += r;
        part_headers_end = find_bytes(buf, have, "\r\n\r\n", 4);
    }

    long data_start = part_headers_end + 4;
    long pending_len = have - data_start;
    if (pending_len > 0) memmove(buf, buf + data_start, pending_len);
    have = pending_len;

    if (!k85_fwflash_stream_begin()) return false;

    bool ok = false;
    long after_file_pos = -1;
    while (true) {
        long dpos = find_bytes(buf, have, delim, delim_len);
        if (dpos >= 0) {
            long write_len = dpos;
            if (write_len >= 2 && buf[write_len - 2] == '\r' && buf[write_len - 1] == '\n') {
                write_len -= 2;
            }
            if (write_len > 0) {
                if (!k85_fwflash_stream_write((const uint8_t *)buf, write_len)) { k85_fwflash_stream_abort(); return false; }
            }
            ok = true;
            after_file_pos = dpos + delim_len;
            break;
        }
        long safe_len = have - (delim_len + 2);
        if (safe_len > 0) {
            if (!k85_fwflash_stream_write((const uint8_t *)buf, safe_len)) { k85_fwflash_stream_abort(); return false; }
            memmove(buf, buf + safe_len, have - safe_len);
            have -= safe_len;
        }
        if (esp_timer_get_time() > s_conn_deadline_us) break;
        if (have >= (long)sizeof(buf)) break;
        int r = recv(conn_fd, buf + have, sizeof(buf) - have, 0);
        if (r <= 0) break;
        have += r;
    }

    if (!ok) {
        k85_fwflash_stream_abort();
        k85_log("firmware upload: incomplete, aborted");
        return false;
    }

    // Разбираем второй multipart-блок (поле sig): оставшиеся данные после
    // границы файла - от него до конца заголовков этой части, потом до
    // следующей границы - это и есть значение подписи.
    if (after_file_pos > 0 && after_file_pos <= have) {
        long remaining = have - after_file_pos;
        if (remaining > 0) memmove(buf, buf + after_file_pos, remaining);
        have = remaining;
    } else {
        have = 0;
    }

    long sig_headers_end = find_bytes(buf, have, "\r\n\r\n", 4);
    while (sig_headers_end < 0) {
        if (esp_timer_get_time() > s_conn_deadline_us) break;
        if (have >= (long)sizeof(buf) - 1) break;
        int r = recv(conn_fd, buf + have, sizeof(buf) - have, 0);
        if (r <= 0) break;
        have += r;
        sig_headers_end = find_bytes(buf, have, "\r\n\r\n", 4);
    }

    char signature_hex[136] = {0};
    if (sig_headers_end >= 0) {
        long sig_data_start = sig_headers_end + 4;
        long sig_pending = have - sig_data_start;
        if (sig_pending > 0) memmove(buf, buf + sig_data_start, sig_pending);
        have = sig_pending;

        long sig_end = find_bytes(buf, have, delim, delim_len);
        while (sig_end < 0 && have < (long)sizeof(buf) - 1) {
            if (esp_timer_get_time() > s_conn_deadline_us) break;
            int r = recv(conn_fd, buf + have, sizeof(buf) - have, 0);
            if (r <= 0) break;
            have += r;
            sig_end = find_bytes(buf, have, delim, delim_len);
        }
        if (sig_end < 0) sig_end = have; // на крайний случай - что есть, то и берём

        long sig_len = sig_end;
        if (sig_len >= 2 && buf[sig_len - 2] == '\r' && buf[sig_len - 1] == '\n') sig_len -= 2;
        if (sig_len > 0 && sig_len < (long)sizeof(signature_hex)) {
            memcpy(signature_hex, buf, sig_len);
            signature_hex[sig_len] = 0;
        }
    }

    if (signature_hex[0] == 0) {
        k85_fwflash_stream_abort();
        k85_log("firmware upload: no signature provided, rejecting");
        return false;
    }

    bool ended = k85_fwflash_stream_end_verified(signature_hex);
    k85_log("firmware upload: %s", ended ? "written to free slot" : "verification/write failed");
    return ended;
}

static bool handle_download(int conn_fd, const char *relpath) {
    if (!sanitize_relpath(relpath)) return false;
    if (is_blocked_download_path(relpath)) {
        k85_log("web-fm: blocked download attempt: %s", relpath);
        return false;
    }
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s/%s", K85_UPLOAD_BASE_PATH, relpath);

    FILE *f = fopen(full_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    const char *base_name = strrchr(relpath, '/');
    base_name = base_name ? base_name + 1 : relpath;

    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %ld\r\nConnection: close\r\n\r\n", base_name, sz);
    send(conn_fd, header, strlen(header), 0);

    char buf[1024];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) send(conn_fd, buf, r, 0);
    fclose(f);
    return true;
}

static void get_parent_dir(const char *dir, char *out, size_t out_size) {
    if (!dir[0]) { out[0] = 0; return; }
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    char *last_slash = strrchr(tmp, '/');
    if (last_slash) *last_slash = 0;
    else tmp[0] = 0;
    snprintf(out, out_size, "%s", tmp);
}

// Ключ больше НЕ передаётся параметром - все ссылки строятся без &key=,
// авторизация полностью на cookie k85key (см. check_access_key выше).
static void build_index_page(char *out, size_t out_size, const char *ssid, const char *dir) {
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    size_t used = 0;
    char parent[192];
    get_parent_dir(dir, parent, sizeof(parent));

    int written0 = snprintf(out, out_size,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>k85OS</title>"
        "<style>body{background:#111;color:#0f0;font-family:monospace;padding:16px;max-width:600px;margin:auto}"
        "h1{color:#0ff}.card{background:#222;padding:12px;margin:10px 0;border-radius:8px;border:1px solid #0ff}"
        "table{width:100%%;border-collapse:collapse;margin-top:10px}"
        "td,th{padding:6px;border-bottom:1px solid #333;text-align:left;font-size:14px}"
        "a{color:#0ff;text-decoration:none;margin-right:8px}"
        "a.del{color:#f66}"
        "a.dir{color:#ff0;font-weight:bold}"
        "a.fw{color:#0f0;font-weight:bold}"
        "input{background:#000;color:#0f0;border:1px solid #0ff;padding:4px;font-family:monospace}"
        "button{background:#0ff;color:#000;border:none;padding:6px 12px;font-family:monospace;cursor:pointer}"
        "</style></head><body>"
        "<h1>k85OS</h1>"
        "<div class='card'>"
        "<b>AP SSID:</b> %s<br>"
        "<b>Uptime:</b> %llds<br>"
        "<b>Free heap:</b> %lu bytes"
        "</div>"
        "<p><a class=fw href='/firmware'>&#9889; Firmware update</a></p>"
        "<p><a href='/upload?dir=%s'>+ Upload file here</a></p>"
        "<form method=GET action=/mkdir style='margin:8px 0'>"
        "<input type=hidden name=dir value='%s'>"
        "<input type=text name=name placeholder='new folder name'>"
        "<button type=submit>+ New folder here</button></form>"
        "<table><tr><th>Name</th><th>Size</th><th>Actions</th></tr>",
        ssid, (long long)uptime_s, (unsigned long)esp_get_free_heap_size(),
        dir, dir);

    if (written0 < 0) return;
    used = strlen(out);

    if (dir[0]) {
        char row[256];
        int w = snprintf(row, sizeof(row),
            "<tr><td colspan=3><a class=dir href='/?dir=%s'>.. (up)</a></td></tr>",
            parent);
        if (w > 0 && used + (size_t)w < out_size - 300) {
            memcpy(out + used, row, w); used += w; out[used] = 0;
        }
    }

    char full_dir[192];
    if (dir[0]) snprintf(full_dir, sizeof(full_dir), "%s/%s", K85_UPLOAD_BASE_PATH, dir);
    else snprintf(full_dir, sizeof(full_dir), "%s", K85_UPLOAD_BASE_PATH);

    DIR *d = opendir(full_dir);
    if (d) {
        struct dirent *ent;
        int count = 0;
        while ((ent = readdir(d)) != nullptr && count < K85_FM_MAX_ENTRIES) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char full[256];
            snprintf(full, sizeof(full), "%s/%s", full_dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

            char child_rel[192];
            if (dir[0]) snprintf(child_rel, sizeof(child_rel), "%s/%s", dir, ent->d_name);
            else snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);

            char row[300];
            int w = snprintf(row, sizeof(row),
                "<tr><td colspan=3><a class=dir href='/?dir=%s'>[%s/]</a></td></tr>",
                child_rel, ent->d_name);
            if (w > 0 && used + (size_t)w < out_size - 300) {
                memcpy(out + used, row, w); used += w; out[used] = 0;
                count++;
            }
        }
        closedir(d);
    }

    d = opendir(full_dir);
    if (d) {
        struct dirent *ent;
        int count = 0;
        while ((ent = readdir(d)) != nullptr && count < K85_FM_MAX_ENTRIES) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char full[256];
            snprintf(full, sizeof(full), "%s/%s", full_dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

            char child_rel[192];
            if (dir[0]) snprintf(child_rel, sizeof(child_rel), "%s/%s", dir, ent->d_name);
            else snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);

            char row[600];
            int written = snprintf(row, sizeof(row),
                "<tr><td>%.40s</td><td>%ldB</td><td>"
                "<a href='/download?name=%.100s'>DL</a>"
                "<form style='display:inline' method=GET action=/rename>"
                "<input type=hidden name=old value='%.100s'>"
                "<input type=hidden name=dir value='%s'>"
                "<input type=text name=new placeholder='new name' style='width:80px'>"
                "<button type=submit>Ren</button></form> "
                "<a class=del href='/delete?name=%.100s' onclick=\"return confirm('Delete %.30s?')\">Del</a>"
                "</td></tr>",
                ent->d_name, (long)st.st_size, child_rel, child_rel, dir, child_rel, ent->d_name);

            if (written > 0 && used + (size_t)written < out_size - 200) {
                memcpy(out + used, row, written);
                used += written;
                out[used] = 0;
                count++;
            }
        }
        closedir(d);
    }

    const char *tail = "</table></body></html>";
    size_t tail_len = strlen(tail);
    if (used + tail_len < out_size) memcpy(out + used, tail, tail_len + 1);
}

static void build_upload_form(char *out, size_t out_size, const char *dir) {
    snprintf(out, out_size,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>k85OS Upload</title>"
        "<style>body{background:#111;color:#0f0;font-family:monospace;padding:20px}"
        "h1{color:#0ff}input,button{font-size:16px;margin-top:10px}"
        "a{color:#0ff}</style></head><body>"
        "<h1>Upload to /littlefs%s%s</h1>"
        "<form method=POST action='/upload?dir=%s' enctype=multipart/form-data>"
        "<input type=file name=file><br>"
        "<button type=submit>Upload</button>"
        "</form>"
        "<p><a href='/?dir=%s'>Back</a></p></body></html>",
        dir[0] ? "/" : "", dir, dir, dir);
}

static void build_firmware_page(char *out, size_t out_size) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *free_slot = k85_fwflash_free_slot_label();

    static char names[K85_FW_LIST_MAX][64];
    static char urls[K85_FW_LIST_MAX][256];
    static char sig_urls[K85_FW_LIST_MAX][256];
    int count = 0;
    bool listed = k85_fwflash_list_available(names, urls, sig_urls, K85_FW_LIST_MAX, &count);

    size_t used = 0;
    int w = snprintf(out, out_size,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>k85OS Firmware</title>"
        "<style>body{background:#000020;color:#eee;font-family:monospace;padding:16px;max-width:600px;margin:auto}"
        "h1{color:#8080ff;border-bottom:1px solid #5555aa;padding-bottom:8px}"
        ".card{background:#000040;padding:12px;margin:10px 0;border-radius:4px;border:1px solid #5555aa}"
        "a{color:#8080ff}"
        "a.fw{display:block;padding:8px;background:#000040;margin:6px 0;border:1px solid #5555aa;text-decoration:none;color:#0f0}"
        "input,button{font-size:16px;margin-top:10px;padding:6px}"
        "input{background:#000;color:#0f0;border:1px solid #8080ff}"
        "button{background:#8080ff;color:#000;border:none;padding:8px 16px;cursor:pointer}"
        "</style></head><body>"
        "<h1>k85OS Setup &mdash; Firmware</h1>"
        "<div class='card'>"
        "<b>Running slot:</b> %s<br>"
        "<b>Free slot:</b> %s<br>"
        "<b>Note:</b> flashing here only WRITES the free slot.<br>"
        "Activate it via device GRUB menu &rarr; \"Alt Firmware\"."
        "</div>",
        running ? running->label : "?", free_slot);
    if (w > 0) used = (size_t)w;

    if (listed && count > 0) {
        int w2 = snprintf(out + used, out_size - used, "<h2>Available from k85OS releases</h2>");
        if (w2 > 0) used += w2;
        for (int i = 0; i < count; i++) {
            int wr = snprintf(out + used, out_size - used,
                "<a class=fw href='/firmware/pull?asset=%d' onclick=\"return confirm('Flash %s into free slot?')\">%s</a>",
                i, names[i], names[i]);
            if (wr > 0 && used + (size_t)wr < out_size) used += wr;
        }
    } else {
        int w2 = snprintf(out + used, out_size - used, "<p>No releases found (or WiFi not connected).</p>");
        if (w2 > 0) used += w2;
    }

    int w3 = snprintf(out + used, out_size - used,
        "<h2>Or upload your own .bin</h2>"
        "<p style='font-size:12px;color:#888'>Requires a valid signature - unsigned builds are refused.</p>"
        "<form method=POST action='/firmware/upload' enctype=multipart/form-data>"
        "<input type=file name=file accept='.bin'><br>"
        "<input type=text name=sig placeholder='signature (128 hex chars)' style='width:100%%;margin-top:6px'><br>"
        "<button type=submit onclick=\"return confirm('Flash uploaded file into free slot?')\">Upload & Flash</button>"
        "</form>"
        "<p><a href='/'>&larr; Back to files</a></p></body></html>");
    if (w3 > 0 && used + (size_t)w3 < out_size) used += w3;
}

static void handle_connection(int conn_fd, const char *ssid) {
    s_conn_deadline_us = esp_timer_get_time() + 20LL * 1000000;
    static char req_buf[4096];
    long req_len = 0;
    long headers_end = -1;

    while (headers_end < 0 && req_len < (long)sizeof(req_buf) - 1) {
        if (esp_timer_get_time() > s_conn_deadline_us) return;
        int r = recv(conn_fd, req_buf + req_len, sizeof(req_buf) - 1 - req_len, 0);
        if (r <= 0) break;
        req_len += r;
        req_buf[req_len] = 0;
        headers_end = find_bytes(req_buf, req_len, "\r\n\r\n", 4);
    }
    if (headers_end < 0) return;

    char method[8] = {0};
    char path_full[256] = {0};
    sscanf(req_buf, "%7s %255s", method, path_full);

    char path[192] = {0};
    char query[256] = {0};
    char *qmark = strchr(path_full, '?');
    if (qmark) {
        size_t plen = (size_t)(qmark - path_full);
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, path_full, plen);
        path[plen] = 0;
        snprintf(query, sizeof(query), "%s", qmark + 1);
    } else {
        snprintf(path, sizeof(path), "%s", path_full);
    }

    bool is_post_early = (strcmp(method, "POST") == 0);
    if (is_post_early && strcmp(path, "/login") == 0) {
        int64_t now_login = esp_timer_get_time();
        if (now_login < s_lockout_until_us) {
            send_http_response(conn_fd, "401 Unauthorized", "text/html", k85_login_page_html);
            return;
        }
        const char *body_ptr = req_buf + headers_end + 4;
        char submitted_key[32] = {0};
        get_query_param(body_ptr, "key", submitted_key, sizeof(submitted_key));

        if (const_time_streq(submitted_key, s_web_access_key)) {
            s_failed_attempts = 0;
            char resp_header[256];
            snprintf(resp_header, sizeof(resp_header),
                "HTTP/1.1 302 Found\r\nSet-Cookie: k85key=%s; Path=/\r\nLocation: /\r\nConnection: close\r\n\r\n",
                s_web_access_key);
            send(conn_fd, resp_header, strlen(resp_header), 0);
            return;
        } else {
            s_failed_attempts++;
            int shift = s_failed_attempts > 5 ? 5 : s_failed_attempts - 1;
            int delay_s = 1 << shift;
            if (delay_s > 30) delay_s = 30;
            s_lockout_until_us = now_login + (int64_t)delay_s * 1000000;
            k85_log("web-fm: bad login POST attempt #%d, locked %ds", s_failed_attempts, delay_s);
            send_http_response(conn_fd, "401 Unauthorized", "text/html", k85_login_page_html);
            return;
        }
    }

    if (!check_access_key(req_buf, (size_t)headers_end, query)) {
        send_http_response(conn_fd, "401 Unauthorized", "text/html", k85_login_page_html);
        return;
    }

    char dir[192] = {0};
    get_query_param(query, "dir", dir, sizeof(dir));
    if (dir[0] && !sanitize_relpath(dir)) dir[0] = 0;

    bool is_post = (strcmp(method, "POST") == 0);

    if (strcmp(path, "/firmware") == 0) {
        static char *body = nullptr;
        if (!body) body = (char *)heap_caps_malloc(6144, MALLOC_CAP_SPIRAM);
        if (!body) body = (char *)heap_caps_malloc(6144, MALLOC_CAP_INTERNAL); // fallback - PSRAM может быть фрагментирована
        if (!body) { send_http_response(conn_fd, "500 Internal Server Error", "text/html", k85_generic_fail_html); return; }
        build_firmware_page(body, 6144);
        send_http_response(conn_fd, "200 OK", "text/html", body);
    } else if (strcmp(path, "/firmware/pull") == 0) {
        char asset_str[8] = {0};
        get_query_param(query, "asset", asset_str, sizeof(asset_str));
        int idx = atoi(asset_str);

        static char names[K85_FW_LIST_MAX][64];
        static char urls[K85_FW_LIST_MAX][256];
        static char sig_urls[K85_FW_LIST_MAX][256];
        int count = 0;
        bool ok = false;
        if (k85_fwflash_list_available(names, urls, sig_urls, K85_FW_LIST_MAX, &count) && idx >= 0 && idx < count) {
            ok = k85_fwflash_from_url(urls[idx], sig_urls[idx], nullptr);
        }
        send_http_response(conn_fd, ok ? "200 OK" : "500 Internal Server Error", "text/html",
                            ok ? "<html><body style='background:#000020;color:#0f0;font-family:monospace;padding:20px'>"
                                 "<h1>Flashed to free slot</h1><p>Activate via device GRUB &rarr; Alt Firmware.</p></body></html>"
                               : k85_generic_fail_html);
    } else if (is_post && strcmp(path, "/firmware/upload") == 0) {
        long body_have = req_len - (headers_end + 4);
        bool ok = handle_firmware_upload_body(conn_fd, req_buf, req_buf + headers_end + 4, body_have);
        send_http_response(conn_fd, ok ? "200 OK" : "400 Bad Request", "text/html",
                            ok ? "<html><body style='background:#000020;color:#0f0;font-family:monospace;padding:20px'>"
                                 "<h1>Flashed to free slot</h1><p>Activate via device GRUB &rarr; Alt Firmware.</p></body></html>"
                               : k85_upload_fail_html);
    } else if (is_post && strcmp(path, "/upload") == 0) {
        long body_have = req_len - (headers_end + 4);
        bool ok = handle_upload_body(conn_fd, req_buf, req_buf + headers_end + 4, body_have, dir);
        send_http_response(conn_fd, ok ? "200 OK" : "400 Bad Request", "text/html",
                            ok ? k85_upload_ok_html : k85_upload_fail_html);
    } else if (strcmp(path, "/upload") == 0) {
        static char *body = nullptr;
        if (!body) body = (char *)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
        if (!body) body = (char *)heap_caps_malloc(512, MALLOC_CAP_INTERNAL);
        if (!body) { send_http_response(conn_fd, "500 Internal Server Error", "text/html", k85_generic_fail_html); return; }
        build_upload_form(body, 512, dir);
        send_http_response(conn_fd, "200 OK", "text/html", body);
    } else if (strcmp(path, "/download") == 0) {
        char name[192] = {0};
        get_query_param(query, "name", name, sizeof(name));
        if (!handle_download(conn_fd, name)) {
            send_http_response(conn_fd, "404 Not Found", "text/html", k85_generic_fail_html);
        }
    } else if (strcmp(path, "/mkdir") == 0) {
        char name[128] = {0};
        get_query_param(query, "name", name, sizeof(name));
        if (sanitize_relpath(name) && name[0]) {
            char full_path[256];
            if (dir[0]) snprintf(full_path, sizeof(full_path), "%s/%s/%s", K85_UPLOAD_BASE_PATH, dir, name);
            else snprintf(full_path, sizeof(full_path), "%s/%s", K85_UPLOAD_BASE_PATH, name);
            mkdir(full_path, 0755);
            k85_log("web-fm: mkdir %s", full_path);
        }
        char redir[256];
        snprintf(redir, sizeof(redir), "/?dir=%s", dir);
        send_http_redirect(conn_fd, redir);
    } else if (strcmp(path, "/delete") == 0) {
        char name[192] = {0};
        get_query_param(query, "name", name, sizeof(name));
        if (sanitize_relpath(name)) {
            char full_path[256];
            snprintf(full_path, sizeof(full_path), "%s/%s", K85_UPLOAD_BASE_PATH, name);
            remove(full_path);
            k85_log("web-fm: deleted %s", full_path);
        }
        char redir[256];
        snprintf(redir, sizeof(redir), "/?dir=%s", dir);
        send_http_redirect(conn_fd, redir);
    } else if (strcmp(path, "/rename") == 0) {
        char old_name[192] = {0}, new_name[128] = {0};
        get_query_param(query, "old", old_name, sizeof(old_name));
        get_query_param(query, "new", new_name, sizeof(new_name));
        if (sanitize_relpath(old_name) && sanitize_relpath(new_name) && new_name[0]) {
            char src[256], dst[256];
            snprintf(src, sizeof(src), "%s/%s", K85_UPLOAD_BASE_PATH, old_name);
            if (dir[0]) snprintf(dst, sizeof(dst), "%s/%s/%s", K85_UPLOAD_BASE_PATH, dir, new_name);
            else snprintf(dst, sizeof(dst), "%s/%s", K85_UPLOAD_BASE_PATH, new_name);
            rename(src, dst);
            k85_log("web-fm: renamed %s -> %s", src, dst);
        }
        char redir[256];
        snprintf(redir, sizeof(redir), "/?dir=%s", dir);
        send_http_redirect(conn_fd, redir);
    } else {
        static char *body = nullptr;
        if (!body) body = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        if (!body) body = (char *)heap_caps_malloc(8192, MALLOC_CAP_INTERNAL); // fallback - PSRAM может быть фрагментирована
        if (!body) {
            ESP_LOGE("k85_web", "wifi_hotspot: body alloc failed (both PSRAM and internal RAM exhausted)");
            return;
        }
        build_index_page(body, 8192, ssid, dir);
        send_http_response(conn_fd, "200 OK", "text/html", body);
    }
}

// Весь цикл accept/handle_connection живёт в отдельной задаче - иначе
// обработка медленного клиента (до 20с на соединение, см. s_conn_deadline_us)
// замораживает UI устройства целиком: кнопки A/B, регенерацию ключа - всё,
// что раньше крутилось в том же цикле, что и сетевой accept().
static void hotspot_server_task(void *arg) {
    while (s_hotspot_task_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s_hotspot_listen_fd, &fds);
        struct timeval tv = {0, 200000};
        int sel = select(s_hotspot_listen_fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(s_hotspot_listen_fd, &fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int conn_fd = accept(s_hotspot_listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (conn_fd >= 0) {
                struct timeval sock_timeout = { .tv_sec = 3, .tv_usec = 0 };
                setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &sock_timeout, sizeof(sock_timeout));
                setsockopt(conn_fd, SOL_SOCKET, SO_SNDTIMEO, &sock_timeout, sizeof(sock_timeout));
                handle_connection(conn_fd, s_hotspot_ssid);
                close(conn_fd);
            }
        }
    }
    vTaskDelete(nullptr);
}

static bool choose_ap_password(char *out_password, size_t out_size) {
    static const char *items[] = {"Auto-generate", "Custom password", "Cancel"};
    int idx = k85_run_list_menu("HOTSPOT PASSWORD", items, 3, nullptr);
    if (idx < 0 || idx == 2) return false;

    if (idx == 0) {
        int len = 8 + (int)(esp_random() % 5);
        gen_random_string(out_password, len, false);
        return true;
    }

    char pass[32] = "";
    if (!k85_text_input("Set hotspot password\n(min 8 chars):", "", pass, sizeof(pass))) return false;
    if (strlen(pass) < 8) {
        k85_show_message("Too short (min 8)\nA+B=back");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return false;
    }
    snprintf(out_password, out_size, "%s", pass);
    return true;
}

void k85_wifi_hotspot_regenerate_key(void) {
    gen_random_string(s_web_access_key, 6 + (int)(esp_random() % 5), false);
    k85_log("web-fm: access key regenerated");
}

void k85_run_wifi_hotspot(void) {
    char password[32];
    if (!choose_ap_password(password, sizeof(password))) return;

    gen_random_string(s_web_access_key, 6 + (int)(esp_random() % 5), false);

    k85_wifi_init();

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "k85OS-%02X%02X", mac[4], mac[5]);

    int channel = g_config.ap_channel;
    if (channel < 1 || channel > 13) channel = 1;

    wifi_config_t ap_config = {};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", ssid);
    ap_config.ap.ssid_len = strlen(ssid);
    if (!g_config.ap_open) {
        snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s", password);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap_config.ap.max_connection = 4;
    ap_config.ap.channel = (uint8_t)channel;

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
    char msg[180];
    auto redraw_hotspot_msg = [&]() {
        if (g_config.ap_open) {
            snprintf(msg, sizeof(msg), "AP: %s (open)\nCh: %d\nIP: %s\nWeb key: %s\nB=new key  A+B=stop",
                     ssid, channel, ip_str, s_web_access_key);
        } else {
            snprintf(msg, sizeof(msg), "AP: %s\nAP pass: %s\nCh: %d\nIP: %s\nWeb key: %s\nB=new key  A+B=stop",
                     ssid, password, channel, ip_str, s_web_access_key);
        }
        k85_show_message(msg);
    };
    redraw_hotspot_msg();

    s_hotspot_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_hotspot_listen_fd < 0) {
        k85_show_message("Socket error");
        esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }
    int opt = 1;
    setsockopt(s_hotspot_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(80);
    bind(s_hotspot_listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(s_hotspot_listen_fd, 2);

    snprintf(s_hotspot_ssid, sizeof(s_hotspot_ssid), "%s", ssid);

    // ВАЖНО: стек этой задачи НЕ может быть в PSRAM - handle_connection
    // внутри читает LittleFS (opendir/fopen), а чтение flash требует
    // временного отключения кэша, что несовместимо с PSRAM-стеком
    // текущей задачи (assert esp_task_stack_is_sane_cache_disabled).
    if (!s_hotspot_task_stack) {
        s_hotspot_task_stack = (StackType_t *)heap_caps_malloc(K85_HOTSPOT_TASK_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    bool task_ok = false;
    if (s_hotspot_task_stack) {
        s_hotspot_task_running = true;
        s_hotspot_task_handle = xTaskCreateStaticPinnedToCore(
            hotspot_server_task, "k85_hotspot_srv", K85_HOTSPOT_TASK_STACK_BYTES, nullptr, 5,
            s_hotspot_task_stack, &s_hotspot_task_buf, tskNO_AFFINITY);
        task_ok = (s_hotspot_task_handle != nullptr);
    }
    if (!task_ok) {
        s_hotspot_task_running = false;
        close(s_hotspot_listen_fd);
        k85_show_message("Server task failed\n(PSRAM OOM?)\nA+B=back");
        esp_wifi_set_mode(WIFI_MODE_STA);
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    // Основной цикл теперь ТОЛЬКО обрабатывает кнопки - сеть крутится в
    // отдельной задаче и не может заморозить этот цикл, что бы ни делал клиент.
    bool running = true;
    while (running) {
        k85_input_update();
        if (k85_ab_held(500)) {
            k85_wait_ab_release();
            running = false;
            break;
        }
        if (k85_btn_b_pressed()) {
            k85_wifi_hotspot_regenerate_key();
            redraw_hotspot_msg();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    s_hotspot_task_running = false;
    vTaskDelay(pdMS_TO_TICKS(300)); // даём задаче заметить флаг и завершиться
    close(s_hotspot_listen_fd);
    s_hotspot_listen_fd = -1;

    esp_wifi_set_mode(WIFI_MODE_STA);
    k85_log("Hotspot stopped");
    k85_show_message("Hotspot stopped");
    vTaskDelay(pdMS_TO_TICKS(1000));
}

#pragma GCC diagnostic pop