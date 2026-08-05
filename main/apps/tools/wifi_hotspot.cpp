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
#include <dirent.h>
#include <sys/stat.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define K85_UPLOAD_BASE_PATH "/littlefs"
#define K85_FM_MAX_FILES 40

static esp_netif_t *s_ap_netif = nullptr;

// ---------- URL decode (%XX Рё +) ----------
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

// Р—Р°С‰РёС‚Р° РѕС‚ РІС‹С…РѕРґР° Р·Р° РїСЂРµРґРµР»С‹ /littlefs С‡РµСЂРµР· "../" РІ РёРјРµРЅРё.
static bool sanitize_relpath(const char *name) {
    if (!name[0]) return false;
    if (strstr(name, "..")) return false;
    if (name[0] == '/') return false;
    return true;
}

static void send_http_response(int fd, const char *status, const char *content_type, const char *body) {
    char header[160];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", status, content_type);
    send(fd, header, strlen(header), 0);
    if (body) send(fd, body, strlen(body), 0);
}

static void send_http_redirect(int fd, const char *location) {
    char header[192];
    snprintf(header, sizeof(header),
             "HTTP/1.1 302 Found\r\nLocation: %s\r\nConnection: close\r\n\r\n", location);
    send(fd, header, strlen(header), 0);
}

// ---------- Р“Р»Р°РІРЅР°СЏ СЃС‚СЂР°РЅРёС†Р°: СЃС‚Р°С‚СѓСЃ + С‚Р°Р±Р»РёС†Р° С„Р°Р№Р»РѕРІ ----------
static void build_index_page(char *out, size_t out_size, const char *ssid, const char *password) {
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    size_t used = 0;
    snprintf(out, out_size,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>k85OS</title>"
        "<style>body{background:#111;color:#0f0;font-family:monospace;padding:16px;max-width:600px;margin:auto}"
        "h1{color:#0ff}.card{background:#222;padding:12px;margin:10px 0;border-radius:8px;border:1px solid #0ff}"
        "table{width:100%%;border-collapse:collapse;margin-top:10px}"
        "td,th{padding:6px;border-bottom:1px solid #333;text-align:left;font-size:14px}"
        "a{color:#0ff;text-decoration:none;margin-right:8px}"
        "a.del{color:#f66}"
        "input{background:#000;color:#0f0;border:1px solid #0ff;padding:4px;font-family:monospace}"
        "button{background:#0ff;color:#000;border:none;padding:6px 12px;font-family:monospace;cursor:pointer}"
        "</style></head><body>"
        "<h1>k85OS</h1>"
        "<div class='card'>"
        "<b>AP SSID:</b> %s<br>"
        "<b>PW:</b> %s<br>"
        "<b>Uptime:</b> %llds<br>"
        "<b>Free heap:</b> %lu bytes"
        "</div>"
        "<p><a href='/upload'>+ Upload file</a></p>"
        "<form method=GET action=/mkdir style='margin:8px 0'>"
        "<input type=text name=name placeholder='new folder name'>"
        "<button type=submit>+ New folder</button></form>"
        "<table><tr><th>Name</th><th>Size</th><th>Actions</th></tr>",
        ssid, password, (long long)uptime_s, (unsigned long)esp_get_free_heap_size());

    used = strlen(out);

    DIR *d = opendir(K85_UPLOAD_BASE_PATH);
    if (d) {
        struct dirent *ent;
        int count = 0;
        while ((ent = readdir(d)) != nullptr && count < K85_FM_MAX_FILES) {
            char full[192];
            snprintf(full, sizeof(full), "%s/%s", K85_UPLOAD_BASE_PATH, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

            char row[512];
            int written = snprintf(row, sizeof(row),
                "<tr><td>%.60s</td><td>%ldB</td><td>"
                "<a href='/download?name=%.60s'>DL</a>"
                "<form style='display:inline' method=GET action=/rename>"
                "<input type=hidden name=old value='%.60s'>"
                "<input type=text name=new placeholder='new name' style='width:90px'>"
                "<button type=submit>Ren</button></form> "
                "<a class=del href='/delete?name=%.60s' onclick=\"return confirm('Delete %.30s?')\">Del</a>"
                "</td></tr>",
                ent->d_name, (long)st.st_size, ent->d_name, ent->d_name, ent->d_name, ent->d_name);

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
    if (used + tail_len < out_size) {
        memcpy(out + used, tail, tail_len + 1);
    }
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
    "<p><a href=/upload style='color:#0ff'>Upload another</a> | <a href=/ style='color:#0ff'>Back</a></p></body></html>";

static const char *k85_upload_fail_html =
    "<!DOCTYPE html><html><body style='background:#111;color:#f66;font-family:monospace;padding:20px'>"
    "<h1>Upload failed</h1><p><a href=/upload style='color:#0ff'>Try again</a></p></body></html>";

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
    if (name[0] == 0) {
        strncpy(name, "upload.bin", 32);
    }
}

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

        long safe_len = have - (delim_len + 2);
        if (safe_len > 0) {
            fwrite(buf, 1, safe_len, f);
            memmove(buf, buf + safe_len, have - safe_len);
            have -= safe_len;
        }
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

// ---------- РЎРєР°С‡РёРІР°РЅРёРµ С„Р°Р№Р»Р° ----------
static bool handle_download(int conn_fd, const char *name) {
    if (!sanitize_relpath(name)) return false;
    char full_path[192];
    snprintf(full_path, sizeof(full_path), "%s/%s", K85_UPLOAD_BASE_PATH, name);

    FILE *f = fopen(full_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %ld\r\nConnection: close\r\n\r\n", name, sz);
    send(conn_fd, header, strlen(header), 0);

    char buf[1024];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        send(conn_fd, buf, r, 0);
    }
    fclose(f);
    return true;
}

// ---------- Р Р°Р·Р±РѕСЂ query-РїР°СЂР°РјРµС‚СЂРѕРІ РІРёРґР° ?a=b&c=d ----------
static bool get_query_param(const char *query, const char *key, char *out, size_t out_size) {
    char search[40];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(query, search);
    if (!p) return false;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char raw[160];
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = 0;
    url_decode(raw, out, out_size);
    return true;
}

static void handle_connection(int conn_fd, const char *ssid, const char *password) {
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
    if (headers_end < 0) return;

    // РџРµСЂРІР°СЏ СЃС‚СЂРѕРєР°: "METHOD /path?query HTTP/1.1"
    char method[8] = {0};
    char path_full[256] = {0};
    sscanf(req_buf, "%7s %255s", method, path_full);

    char path[192] = {0};
    char query[192] = {0};
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

    bool is_post = (strcmp(method, "POST") == 0);

    if (is_post && strcmp(path, "/upload") == 0) {
        long body_have = req_len - (headers_end + 4);
        bool ok = handle_upload_body(conn_fd, req_buf, req_buf + headers_end + 4, body_have);
        send_http_response(conn_fd, ok ? "200 OK" : "400 Bad Request", "text/html",
                            ok ? k85_upload_ok_html : k85_upload_fail_html);
    } else if (strcmp(path, "/upload") == 0) {
        send_http_response(conn_fd, "200 OK", "text/html", k85_upload_form_html);
    } else if (strcmp(path, "/download") == 0) {
        char name[128] = {0};
        get_query_param(query, "name", name, sizeof(name));
        if (!handle_download(conn_fd, name)) {
            send_http_response(conn_fd, "404 Not Found", "text/html", k85_generic_fail_html);
        }
    } else if (strcmp(path, "/delete") == 0) {
        char name[128] = {0};
        get_query_param(query, "name", name, sizeof(name));
        if (sanitize_relpath(name)) {
            char full_path[192];
            snprintf(full_path, sizeof(full_path), "%s/%s", K85_UPLOAD_BASE_PATH, name);
            remove(full_path);
            k85_log("web-fm: deleted %s", full_path);
        }
        send_http_redirect(conn_fd, "/");
    } else if (strcmp(path, "/mkdir") == 0) {
        char name[128] = {0};
        get_query_param(query, "name", name, sizeof(name));
        if (sanitize_relpath(name) && name[0]) {
            char full_path[192];
            snprintf(full_path, sizeof(full_path), "%s/%s", K85_UPLOAD_BASE_PATH, name);
            mkdir(full_path, 0755);
            k85_log("web-fm: mkdir %s", full_path);
        }
        send_http_redirect(conn_fd, "/");
    } else if (strcmp(path, "/rename") == 0) {
        char old_name[128] = {0}, new_name[128] = {0};
        get_query_param(query, "old", old_name, sizeof(old_name));
        get_query_param(query, "new", new_name, sizeof(new_name));
        if (sanitize_relpath(old_name) && sanitize_relpath(new_name) && new_name[0]) {
            char src[192], dst[192];
            snprintf(src, sizeof(src), "%s/%s", K85_UPLOAD_BASE_PATH, old_name);
            snprintf(dst, sizeof(dst), "%s/%s", K85_UPLOAD_BASE_PATH, new_name);
            rename(src, dst);
            k85_log("web-fm: renamed %s -> %s", src, dst);
        }
        send_http_redirect(conn_fd, "/");
    } else {
        static char body[6144];
        build_index_page(body, sizeof(body), ssid, password);
        send_http_response(conn_fd, "200 OK", "text/html", body);
    }
}

void k85_run_wifi_hotspot(void) {
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
    char msg[96];
    snprintf(msg, sizeof(msg), "AP: %s\nPW: %s\nIP: %s\nA+B=stop", ssid, password, ip_str);
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
        struct timeval tv = {0, 200000};
        int sel = select(listen_fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(listen_fd, &fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (conn_fd >= 0) {
                handle_connection(conn_fd, ssid, password);
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

#pragma GCC diagnostic pop
