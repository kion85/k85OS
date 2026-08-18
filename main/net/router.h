#pragma once
#include <stdbool.h>
#include <stdint.h>

#define K85_ROUTER_LOG_MAX 32
#define K85_ROUTER_ACL_MAX 16
#define K85_DNS_RULES_MAX 24

typedef struct {
    char mac_str[18];
    int64_t timestamp_us;
    bool connected; // true = connect event, false = disconnect
} K85RouterLogEntry;

bool k85_router_start(const char *ap_ssid, const char *ap_password, bool ap_open, bool is_guest);
void k85_router_stop(void);
bool k85_router_is_running(void);
bool k85_router_sta_connected(void); // получен ли IP от аплинка

int  k85_router_get_log(K85RouterLogEntry out[], int max);

// ACL: пустой список = разрешены все. Иначе — allow-list (только эти MAC могут подключиться).
bool k85_router_acl_add(const char *mac_str);
bool k85_router_acl_remove(int idx);
int  k85_router_acl_list(char out[][18], int max);
void k85_router_acl_clear(void);

// DNS-фильтр: правила "MAC (или *) -> домен(-суффикс)". Пустой список = ничего не блокируется.
bool k85_router_dns_rule_add(const char *mac_or_star, const char *domain_suffix);
bool k85_router_dns_rule_remove(int idx);
int  k85_router_dns_rule_list(char out_mac[][18], char out_domain[][64], int max);

