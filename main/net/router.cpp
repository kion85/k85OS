#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "router.h"
#include "core/config.h"
#include "core/log.h"
#include "wifi.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <cstring>
#include <cstdio>

static const char *TAG = "k85_router";

static void start_dns_filter(void);
static void stop_dns_filter(void);
static TaskHandle_t s_dns_task_handle = nullptr;
static volatile bool s_dns_running = false;

static esp_netif_t *s_ap_netif = nullptr;
static esp_netif_t *s_sta_netif = nullptr;
static bool s_running = false;
static bool s_sta_got_ip = false;
static bool s_napt_enabled = false;

static K85RouterLogEntry s_log[K85_ROUTER_LOG_MAX];
static int s_log_count = 0;
static int s_log_next = 0;

static char s_acl[K85_ROUTER_ACL_MAX][18];
static int s_acl_count = 0;

static void mac_to_str(const uint8_t *mac, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void log_add(const char *mac_str, bool connected) {
    K85RouterLogEntry *e = &s_log[s_log_next];
    snprintf(e->mac_str, sizeof(e->mac_str), "%s", mac_str);
    e->timestamp_us = esp_timer_get_time();
    e->connected = connected;
    s_log_next = (s_log_next + 1) % K85_ROUTER_LOG_MAX;
    if (s_log_count < K85_ROUTER_LOG_MAX) s_log_count++;
}

static bool acl_is_allowed(const char *mac_str) {
    if (s_acl_count == 0) return true; // ?????? ACL = ????????? ???
    for (int i = 0; i < s_acl_count; i++) {
        if (strcasecmp(s_acl[i], mac_str) == 0) return true;
    }
    return false;
}

static void try_enable_napt(void) {
    if (s_napt_enabled || !s_sta_got_ip || !s_ap_netif) return;
    esp_err_t err = esp_netif_napt_enable(s_ap_netif);
    if (err == ESP_OK) {
        s_napt_enabled = true;
        k85_log("router: NAPT enabled, internet sharing active");
        ESP_LOGI(TAG, "NAPT enabled on AP interface");
    } else {
        ESP_LOGE(TAG, "esp_netif_napt_enable failed: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
        char mac_str[18];
        mac_to_str(ev->mac, mac_str);
        if (!acl_is_allowed(mac_str)) {
            esp_wifi_deauth_sta(ev->aid);
            k85_log("router: rejected %s (not in ACL)", mac_str);
            return;
        }
        log_add(mac_str, true);
        k85_log("router: client connected %s", mac_str);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)event_data;
        char mac_str[18];
        mac_to_str(ev->mac, mac_str);
        log_add(mac_str, false);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_sta_got_ip = true;
        try_enable_napt();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_got_ip = false;
        esp_wifi_connect(); // ????-????????? ???????
    }
}

bool k85_router_start(const char *ap_ssid, const char *ap_password, bool ap_open, bool is_guest) {
    if (s_running) return false;

    if (!g_config.wifi_saved || g_config.wifi_ssid[0] == '\0') {
        k85_log("router: no saved uplink WiFi, cannot start");
        return false;
    }

    k85_wifi_init(); // ?????????????? ??? ???????????? STA-????????????? (netif/event loop/esp_wifi), ??? ?????????? ????????

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    static bool s_handlers_registered = false;
    if (!s_handlers_registered) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr);
        s_handlers_registered = true;
    }

    k85_log("router: free heap before APSTA start: %lu KB", (unsigned long)(esp_get_free_heap_size() / 1024));
    ESP_LOGI(TAG, "free heap before APSTA start: %lu KB", (unsigned long)(esp_get_free_heap_size() / 1024));

    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t sta_cfg = {};
    snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%s", g_config.wifi_ssid);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", g_config.wifi_password);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

    wifi_config_t ap_cfg = {};
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", ap_ssid);
    ap_cfg.ap.ssid_len = strlen(ap_ssid);
    if (!ap_open) {
        snprintf((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "%s", ap_password);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap_cfg.ap.max_connection = 4;
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    esp_wifi_start();
    esp_wifi_connect();

    s_running = true;
    s_sta_got_ip = false;
    s_napt_enabled = false;
    start_dns_filter();
    k85_log("router: started (%s mode), uplink=%s", is_guest ? "guest" : "home", g_config.wifi_ssid);
    return true;
}

void k85_router_stop(void) {
    if (!s_running) return;
    esp_wifi_stop();
    s_running = false;
    s_sta_got_ip = false;
    s_napt_enabled = false;
    stop_dns_filter();
    k85_log("router: stopped");
}

bool k85_router_is_running(void) { return s_running; }
bool k85_router_sta_connected(void) { return s_sta_got_ip; }

int k85_router_get_log(K85RouterLogEntry out[], int max) {
    int n = s_log_count < max ? s_log_count : max;
    for (int i = 0; i < n; i++) {
        int idx = (s_log_next - 1 - i + K85_ROUTER_LOG_MAX) % K85_ROUTER_LOG_MAX;
        out[i] = s_log[idx];
    }
    return n;
}

bool k85_router_acl_add(const char *mac_str) {
    if (s_acl_count >= K85_ROUTER_ACL_MAX) return false;
    snprintf(s_acl[s_acl_count], sizeof(s_acl[s_acl_count]), "%s", mac_str);
    s_acl_count++;
    return true;
}

bool k85_router_acl_remove(int idx) {
    if (idx < 0 || idx >= s_acl_count) return false;
    for (int i = idx; i < s_acl_count - 1; i++) {
        memcpy(s_acl[i], s_acl[i + 1], sizeof(s_acl[i]));
    }
    s_acl_count--;
    return true;
}

int k85_router_acl_list(char out[][18], int max) {
    int n = s_acl_count < max ? s_acl_count : max;
    for (int i = 0; i < n; i++) memcpy(out[i], s_acl[i], sizeof(out[i]));
    return n;
}

void k85_router_acl_clear(void) { s_acl_count = 0; }

// ============================ DNS-?????? (per-MAC domain blocklist) ============================
struct K85DnsRule { char mac[18]; char domain[64]; };
static K85DnsRule s_dns_rules[K85_DNS_RULES_MAX];
static int s_dns_rules_count = 0;

static bool resolve_mac_for_ip(uint32_t ip_raw, char *mac_out) {
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK || sta_list.num == 0) return false;

    esp_netif_pair_mac_ip_t pairs[16];
    int n = sta_list.num > 16 ? 16 : sta_list.num;
    for (int i = 0; i < n; i++) memcpy(pairs[i].mac, sta_list.sta[i].mac, 6);

    if (esp_netif_dhcps_get_clients_by_mac(s_ap_netif, n, pairs) != ESP_OK) return false;

    for (int i = 0; i < n; i++) {
        if (pairs[i].ip.addr == ip_raw) {
            mac_to_str(pairs[i].mac, mac_out);
            return true;
        }
    }
    return false;
}

static bool domain_suffix_match(const char *qname, const char *rule_domain) {
    size_t qlen = strlen(qname), rlen = strlen(rule_domain);
    if (rlen == 0 || rlen > qlen) return false;
    if (strcasecmp(qname + (qlen - rlen), rule_domain) != 0) return false;
    if (qlen == rlen) return true;
    return qname[qlen - rlen - 1] == '.';
}

static bool dns_is_blocked(const char *client_mac, const char *qname) {
    for (int i = 0; i < s_dns_rules_count; i++) {
        bool mac_match = (s_dns_rules[i].mac[0] == '*') || (strcasecmp(s_dns_rules[i].mac, client_mac) == 0);
        if (mac_match && domain_suffix_match(qname, s_dns_rules[i].domain)) return true;
    }
    return false;
}

static bool parse_dns_qname(const uint8_t *buf, int len, char *out, size_t out_size, int *question_end) {
    if (len < 12) return false;
    int pos = 12;
    size_t oi = 0;
    while (pos < len && buf[pos] != 0) {
        int lab_len = buf[pos];
        if (lab_len > 63 || pos + 1 + lab_len >= len) return false;
        pos++;
        if (oi > 0 && oi + 1 < out_size) out[oi++] = '.';
        for (int i = 0; i < lab_len && oi + 1 < out_size; i++) out[oi++] = (char)buf[pos + i];
        pos += lab_len;
    }
    out[oi] = 0;
    if (pos >= len) return false;
    pos++;
    pos += 4;
    if (pos > len) return false;
    *question_end = pos;
    return true;
}

static void send_nxdomain(int sock, const struct sockaddr_in *client_addr, uint8_t *req, int question_end) {
    req[2] = (req[2] & 0x79) | 0x80;
    req[3] = 0x80 | 0x03;
    req[6] = 0; req[7] = 0;
    req[8] = 0; req[9] = 0;
    req[10] = 0; req[11] = 0;
    sendto(sock, req, question_end, 0, (const struct sockaddr *)client_addr, sizeof(*client_addr));
}

static void forward_to_upstream(int sock, const struct sockaddr_in *client_addr, uint8_t *req, int req_len) {
    esp_netif_dns_info_t dns_info = {};
    uint32_t upstream_ip = 0;
    if (s_sta_netif && esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
        dns_info.ip.u_addr.ip4.addr != 0) {
        upstream_ip = dns_info.ip.u_addr.ip4.addr;
    } else {
        upstream_ip = htonl(0x08080808);
    }

    int up_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (up_sock < 0) return;
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(up_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in up_addr = {};
    up_addr.sin_family = AF_INET;
    up_addr.sin_port = htons(53);
    up_addr.sin_addr.s_addr = upstream_ip;

    sendto(up_sock, req, req_len, 0, (const struct sockaddr *)&up_addr, sizeof(up_addr));

    uint8_t resp[512];
    int r = recv(up_sock, resp, sizeof(resp), 0);
    if (r > 0) {
        sendto(sock, resp, r, 0, (const struct sockaddr *)client_addr, sizeof(*client_addr));
    }
    close(up_sock);
}

static void dns_filter_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(nullptr); return; }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[512];
    while (s_dns_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &client_len);
        if (len <= 0) continue;

        char qname[130];
        int question_end = 0;
        if (!parse_dns_qname(buf, len, qname, sizeof(qname), &question_end)) continue;

        char client_mac[18] = "??:??:??:??:??:??";
        resolve_mac_for_ip(client_addr.sin_addr.s_addr, client_mac);

        if (dns_is_blocked(client_mac, qname)) {
            k85_log("dns-filter: blocked %s for %s", qname, client_mac);
            send_nxdomain(sock, &client_addr, buf, question_end);
        } else {
            forward_to_upstream(sock, &client_addr, buf, len);
        }
    }

    close(sock);
    s_dns_task_handle = nullptr;
    vTaskDelete(nullptr);
}

static void start_dns_filter(void) {
    if (s_dns_task_handle) return;
    s_dns_running = true;
    xTaskCreate(dns_filter_task, "k85_dns_filter", 4096, nullptr, 5, &s_dns_task_handle);
}

static void stop_dns_filter(void) {
    if (!s_dns_task_handle) return;
    s_dns_running = false;
    vTaskDelay(pdMS_TO_TICKS(600));
}

bool k85_router_dns_rule_add(const char *mac_or_star, const char *domain_suffix) {
    if (s_dns_rules_count >= K85_DNS_RULES_MAX) return false;
    snprintf(s_dns_rules[s_dns_rules_count].mac, sizeof(s_dns_rules[s_dns_rules_count].mac), "%s", mac_or_star);
    snprintf(s_dns_rules[s_dns_rules_count].domain, sizeof(s_dns_rules[s_dns_rules_count].domain), "%s", domain_suffix);
    s_dns_rules_count++;
    return true;
}

bool k85_router_dns_rule_remove(int idx) {
    if (idx < 0 || idx >= s_dns_rules_count) return false;
    for (int i = idx; i < s_dns_rules_count - 1; i++) s_dns_rules[i] = s_dns_rules[i + 1];
    s_dns_rules_count--;
    return true;
}

int k85_router_dns_rule_list(char out_mac[][18], char out_domain[][64], int max) {
    int n = s_dns_rules_count < max ? s_dns_rules_count : max;
    for (int i = 0; i < n; i++) {
        memcpy(out_mac[i], s_dns_rules[i].mac, sizeof(out_mac[i]));
        memcpy(out_domain[i], s_dns_rules[i].domain, sizeof(out_domain[i]));
    }
    return n;
}

#pragma GCC diagnostic pop








