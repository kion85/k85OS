#include "web_terminal.h"
#include "../../net/wifi.h"
#include "../../core/shell_commands.h"
#include "list_menu.h"
#include "text_input.h"
#include "common.h"
#include "input.h"
#include "log.h"

#include "M5Unified.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static char s_access_key[16];
static int s_failed_attempts = 0;
static int64_t s_lockout_until_us = 0;
static int64_t s_conn_deadline_us = 0;

static esp_netif_t *s_ap_netif = nullptr;
static volatile bool s_server_running = false;

static bool const_time_streq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    volatile unsigned char diff = (unsigned char)(la != lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

static void gen_access_key(void) {
    static const char *charset = "ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    int cs_len = (int)strlen(charset);
    int len = 6 + (int)(esp_random() % 5);
    for (int i = 0; i < len; i++) s_access_key[i] = charset[esp_random() % cs_len];
    s_access_key[len] = 0;
}

static long find_bytes(const char *buf, long len, const char *needle, long needle_len) {
    if (needle_len <= 0 || len < needle_len) return -1;
    for (long i = 0; i <= len - needle_len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) return i;
    }
    return -1;
}

static bool get_cookie_value(const char *headers_block, size_t headers_len, const char *name, char *out, size_t out_size) {
    const char *cookie_hdr = nullptr;
    for (size_t i = 0; i + 7 < headers_len; i++) {
        if (strncasecmp(headers_block + i, "Cookie:", 7) == 0) { cookie_hdr = headers_block + i + 7; break; }
    }
    if (!cookie_hdr) return false;
    char name_eq[32];
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

static bool get_param(const char *query, const char *key, char *out, size_t out_size) {
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(query, search);
    if (!p) return false;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
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

static void send_http(int fd, const char *status, const char *ctype, const char *body) {
    char header[160];
    snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", status, ctype);
    send(fd, header, strlen(header), 0);
    if (body) send(fd, body, strlen(body), 0);
}

static const char *K85_LOGIN_HTML =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>k85OS Terminal</title>"
    "<style>body{background:#000;color:#0f0;font-family:monospace;padding:20px;max-width:400px;margin:auto}"
    "h1{color:#0ff}input,button{font-size:16px;margin-top:10px;padding:6px;width:100%;box-sizing:border-box}"
    "input{background:#000;color:#0f0;border:1px solid #0ff}"
    "button{background:#0ff;color:#000;border:none;padding:8px 16px;cursor:pointer}"
    "</style></head><body>"
    "<h1>k85OS Web Terminal</h1>"
    "<p>Enter the access code shown on the device.</p>"
    "<form method=POST action=/login>"
    "<input type=text name=key placeholder='access code' autofocus>"
    "<button type=submit>Enter</button>"
    "</form></body></html>";

static const char *K85_TERM_PAGE = R"HTML(<!DOCTYPE html><html><head><meta charset='UTF-8'>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>k85OS Terminal</title>
<style>
body{background:#0a0a0a;color:#0f0;font-family:monospace;margin:0;padding:12px;max-width:600px;margin:auto}
h1{color:#0ff;font-size:16px;margin-bottom:8px}
#out{background:#000;border:1px solid #0f0;border-radius:4px;padding:8px;height:60vh;overflow-y:auto;white-space:pre-wrap;font-size:13px;line-height:1.4}
.row{display:flex;gap:6px;margin-top:8px}
#cmd{flex:1;background:#000;color:#0f0;border:1px solid #0f0;padding:8px;font-family:monospace;font-size:14px}
button{background:#0f0;color:#000;border:none;padding:8px 14px;font-family:monospace;cursor:pointer;font-weight:bold}
</style></head><body>
<h1>k85OS $ terminal</h1>
<div id="out">Type 'help' for commands.\n</div>
<div class="row">
  <input id="cmd" placeholder="command" autofocus>
  <button onclick="sendCmd()">Run</button>
</div>
<script>
const out = document.getElementById('out');
const cmdBox = document.getElementById('cmd');

function appendLine(text) {
  out.textContent += text;
  out.scrollTop = out.scrollHeight;
}

async function sendCmd() {
  const cmd = cmdBox.value;
  if (!cmd) return;
  appendLine('\n$ ' + cmd + '\n');
  cmdBox.value = '';
  try {
    const r = await fetch('/api/exec?cmd=' + encodeURIComponent(cmd));
    const j = await r.json();
    appendLine(j.output || '(no output)');
  } catch(e) {
    appendLine('(connection error)\n');
  }
}

cmdBox.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') sendCmd();
});
</script>
</body></html>)HTML";

static void handle_connection(int conn_fd) {
    static char req_buf[2048];
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

    char method[8] = {0}, path_full[256] = {0};
    sscanf(req_buf, "%7s %255s", method, path_full);

    char path[256] = {0}, query[256] = {0};
    char *q = strchr(path_full, '?');
    if (q) {
        size_t plen = (size_t)(q - path_full);
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, path_full, plen); path[plen] = 0;
        snprintf(query, sizeof(query), "%s", q + 1);
    } else {
        snprintf(path, sizeof(path), "%s", path_full);
    }

    bool is_post = (strcmp(method, "POST") == 0);

    if (is_post && strcmp(path, "/login") == 0) {
        int64_t now = esp_timer_get_time();
        if (now < s_lockout_until_us) {
            send_http(conn_fd, "401 Unauthorized", "text/html", K85_LOGIN_HTML);
            return;
        }
        const char *body_ptr = req_buf + headers_end + 4;
        char submitted[32] = {0};
        get_param(body_ptr, "key", submitted, sizeof(submitted));

        if (const_time_streq(submitted, s_access_key)) {
            s_failed_attempts = 0;
            char resp[256];
            snprintf(resp, sizeof(resp),
                "HTTP/1.1 302 Found\r\nSet-Cookie: k85term=%s; Path=/\r\nLocation: /\r\nConnection: close\r\n\r\n",
                s_access_key);
            send(conn_fd, resp, strlen(resp), 0);
        } else {
            s_failed_attempts++;
            int shift = s_failed_attempts > 5 ? 5 : s_failed_attempts - 1;
            int delay_s = 1 << shift;
            if (delay_s > 30) delay_s = 30;
            s_lockout_until_us = now + (int64_t)delay_s * 1000000;
            k85_log("web-term: bad login attempt #%d, locked %ds", s_failed_attempts, delay_s);
            send_http(conn_fd, "401 Unauthorized", "text/html", K85_LOGIN_HTML);
        }
        return;
    }

    char cookie_key[32] = {0};
    bool authed = get_cookie_value(req_buf, (size_t)headers_end, "k85term", cookie_key, sizeof(cookie_key))
                  && const_time_streq(cookie_key, s_access_key);
    if (!authed) {
        send_http(conn_fd, "401 Unauthorized", "text/html", K85_LOGIN_HTML);
        return;
    }

    if (strcmp(path, "/") == 0) {
        send_http(conn_fd, "200 OK", "text/html", K85_TERM_PAGE);
    } else if (strcmp(path, "/api/exec") == 0) {
        char cmd_enc[80] = {0}, cmd[80] = {0};
        get_param(query, "cmd", cmd_enc, sizeof(cmd_enc));
        url_decode(cmd_enc, cmd, sizeof(cmd));

        char resp[512];
        k85_shell_run_command(cmd, resp, sizeof(resp));

        // Экранируем для JSON: кавычки, обратные слэши, переносы строк
        char json_out[1024];
        size_t oi = 0;
        json_out[oi++] = '{';
        oi += snprintf(json_out + oi, sizeof(json_out) - oi, "\"output\":\"");
        for (const char *p = resp; *p && oi < sizeof(json_out) - 8; p++) {
            if (*p == '"' || *p == '\\') { json_out[oi++] = '\\'; json_out[oi++] = *p; }
            else if (*p == '\r') continue;
            else if (*p == '\n') { json_out[oi++] = '\\'; json_out[oi++] = 'n'; }
            else json_out[oi++] = *p;
        }
        oi += snprintf(json_out + oi, sizeof(json_out) - oi, "\"}");
        json_out[oi] = 0;

        send_http(conn_fd, "200 OK", "application/json", json_out);
    } else {
        send_http(conn_fd, "404 Not Found", "text/plain", "Not found");
    }
}

static bool setup_ap_and_get_choice(bool *out_open, char *out_pass, size_t pass_size, int *out_channel) {
    const char *sec_items[] = { "Open (no password)", "WPA2 (set password)" };
    int sec = k85_run_list_menu("ENCRYPTION", sec_items, 2, nullptr);
    if (sec < 0) return false;
    *out_open = (sec == 0);

    if (!*out_open) {
        if (!k85_text_input("AP password\n(min 8 chars):", "", out_pass, pass_size)) return false;
        if (strlen(out_pass) < 8) {
            k85_show_message("Too short (min 8)\nA+B=back");
            vTaskDelay(pdMS_TO_TICKS(1500));
            return false;
        }
    } else {
        out_pass[0] = 0;
    }

    char ch_buf[4] = "1";
    if (!k85_text_input("Channel (1-13):", ch_buf, ch_buf, sizeof(ch_buf))) return false;
    int ch = atoi(ch_buf);
    if (ch < 1 || ch > 13) ch = 1;
    *out_channel = ch;
    return true;
}

void k85_run_web_terminal(void) {
    const char *net_items[] = { "Use current WiFi", "Create own AP (k85os-term)" };
    int net_choice = k85_run_list_menu("NETWORK MODE", net_items, 2, nullptr);
    if (net_choice < 0) return;

    bool use_own_ap = (net_choice == 1);
    char ip_str[16] = "";

    if (!use_own_ap) {
        if (!k85_wifi_is_connected()) {
            k85_show_message("Connecting to\nsaved WiFi...");
            bool ok = k85_wifi_connect_saved();
            if (!ok) {
                k85_show_message("Not connected\nSetup WiFi first\n(WiFi Manager)\nA+B=back");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                return;
            }
        }
        snprintf(ip_str, sizeof(ip_str), "%s", k85_wifi_get_ip_str());
    } else {
        bool ap_open = true;
        char ap_pass[32] = "";
        int ap_channel = 1;
        if (!setup_ap_and_get_choice(&ap_open, ap_pass, sizeof(ap_pass), &ap_channel)) return;

        if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

        wifi_config_t ap_config = {};
        const char *ssid = "k85os-term";
        snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", ssid);
        ap_config.ap.ssid_len = strlen(ssid);
        if (!ap_open) {
            snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s", ap_pass);
            ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ap_config.ap.authmode = WIFI_AUTH_OPEN;
        }
        ap_config.ap.max_connection = 4;
        ap_config.ap.channel = (uint8_t)ap_channel;

        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        esp_err_t start_err = esp_wifi_start();
        if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_STATE) {
            k85_show_message("AP start failed\nA+B=back");
            vTaskDelay(pdMS_TO_TICKS(1500));
            return;
        }

        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(s_ap_netif, &ip_info);
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    gen_access_key();

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(80);
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 2);

    char msg[160];
    snprintf(msg, sizeof(msg), "%s\nIP: %s\nKey: %s\nB=new key A+B=stop",
             use_own_ap ? "Own AP mode" : "Home network", ip_str, s_access_key);
    k85_show_message(msg);

    s_server_running = true;
    while (s_server_running) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); s_server_running = false; break; }
        if (k85_btn_b_pressed()) {
            gen_access_key();
            snprintf(msg, sizeof(msg), "%s\nIP: %s\nKey: %s\nB=new key A+B=stop",
                     use_own_ap ? "Own AP mode" : "Home network", ip_str, s_access_key);
            k85_show_message(msg);
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
                s_conn_deadline_us = esp_timer_get_time() + 10LL * 1000000;
                struct timeval sock_tv = { .tv_sec = 5, .tv_usec = 0 };
                setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &sock_tv, sizeof(sock_tv));
                setsockopt(conn_fd, SOL_SOCKET, SO_SNDTIMEO, &sock_tv, sizeof(sock_tv));
                handle_connection(conn_fd);
                close(conn_fd);
            }
        }
    }
    close(listen_fd);

    if (use_own_ap) esp_wifi_set_mode(WIFI_MODE_STA);
    k85_show_message("Web Terminal stopped");
    vTaskDelay(pdMS_TO_TICKS(1000));
}