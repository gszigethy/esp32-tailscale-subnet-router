#!/usr/bin/env python3
"""Host regressions for W5500 integration with upstream exit-node hosting.

Compile the production helpers with small ESP/NVS stubs; no device changes.
Run with: python3 tools/test_w5500_rebase.py
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def compile_run(name: str, source: str) -> None:
    with tempfile.TemporaryDirectory(prefix="w5500-test-") as directory:
        path = Path(directory) / f"{name}.c"
        path.write_text(source)
        binary = path.with_suffix("")
        subprocess.run(
            ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


def test_routes() -> None:
    source = (ROOT / "main/tailscale_manager.c").read_text()
    helpers = "\n".join(function(source, signature) for signature in [
        "bool tailscale_exit_server_active(void)",
        "static size_t routes_append_unique(",
        "const char *tailscale_advertise_routes_effective(void)",
        "static bool fork_routes_append_unique(",
        "static bool routes_append_cached(",
        "const char *tailscale_compose_routes(void)",
    ])
    compile_run("routes", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_LOGW(...) ((void)0)
int tailscale_enabled = 1, tailscale_advertise_exit_node = 0;
uint32_t tailscale_exit_node_ip = 0;
int tailscale_advertise_ap = 1;
char *tailscale_advertise_routes = NULL;
volatile uint8_t eth_route_en = 1, sta_route_en = 0;
volatile int eth_connect = 1, sta_connect = 0;
static bool ap_cidr_from_nvs(char *out, size_t capacity) {
    snprintf(out, capacity, "192.168.4.0/24"); return true;
}
static char *nvs_param_get_str(const char *key) {
    return strdup(strcmp(key, "eth_route_cidr") == 0 ? "192.168.1.0/24" : "192.168.2.0/24");
}
''' + helpers + r'''
static int occurrences(const char *list, const char *route) {
    int count = 0;
    while (list && *list) {
        const char *end = strchr(list, '\n');
        size_t length = end ? (size_t)(end-list) : strlen(list);
        if (length == strlen(route) && strncmp(list, route, length) == 0) count++;
        list = end ? end + 1 : NULL;
    }
    return count;
}
int main(void) {
    const char *routes = tailscale_compose_routes();
    assert(occurrences(routes, "192.168.1.0/24") == 1);
    assert(occurrences(routes, "192.168.4.0/24") == 1);
    assert(occurrences(routes, "0.0.0.0/0") == 0);
    tailscale_advertise_exit_node = 1;
    tailscale_advertise_routes = "192.168.1.0/24\n0.0.0.0/0\n::/0";
    routes = tailscale_compose_routes();
    assert(occurrences(routes, "192.168.1.0/24") == 1);
    assert(occurrences(routes, "0.0.0.0/0") == 1);
    assert(occurrences(routes, "::/0") == 1);
    char many[600] = "";
    for (int i = 0; i < 35; i++) {
        char route[32]; snprintf(route, sizeof route, "10.%d.0.0/16\n", i);
        strcat(many, route);
    }
    tailscale_advertise_routes = many;
    routes = tailscale_compose_routes();
    assert(strlen(routes) <= 255);
    assert(occurrences(routes, "0.0.0.0/0") == 1);
    assert(occurrences(routes, "::/0") == 1);
    assert(occurrences(routes, "192.168.1.0/24") == 1);
    tailscale_advertise_routes = NULL;
    tailscale_exit_node_ip = 123;
    assert(!tailscale_exit_server_active());
    assert(occurrences(tailscale_compose_routes(), "0.0.0.0/0") == 0);
    tailscale_exit_node_ip = 0;
    tailscale_advertise_exit_node = 0;
    eth_connect = 0; sta_connect = 1; sta_route_en = 1;
    routes = tailscale_compose_routes();
    assert(occurrences(routes, "192.168.1.0/24") == 0);
    assert(occurrences(routes, "192.168.2.0/24") == 1);
    sta_connect = 0; tailscale_advertise_ap = 0;
    assert(tailscale_compose_routes() == NULL);
    puts("PASS: route composition, defaults under overflow, deduplication, failover");
}
''')


def test_dns() -> None:
    source = (ROOT / "main/peer_dns.c").read_text()
    helper = function(source, "static uint32_t select_resolver(void)")
    stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define ESP_NETIF_DNS_MAIN 0
#define ESP_IPADDR_TYPE_V4 0
typedef struct { uint32_t dns; } esp_netif_t;
typedef struct { struct { int type; union { struct { uint32_t addr; } ip4; } u_addr; } ip; } esp_netif_dns_info_t;
volatile int eth_connect = 0, sta_connect = 0;
static esp_netif_t eth = {11}, sta = {22};
static uint32_t custom = 0;
static uint32_t dns_relay_get_upstream(void) { return custom; }
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    return strcmp(key, "ETH_DEF") == 0 ? &eth : &sta;
}
static int esp_netif_get_dns_info(esp_netif_t *netif, int kind, esp_netif_dns_info_t *info) {
    (void)kind; info->ip.u_addr.ip4.addr = netif->dns; return ESP_OK;
}
static bool resolver_is_usable(uint32_t ip) { return ip != 0 && ip != 99; }
'''
    compile_run("dns_wired", "#define CONFIG_ETH_W5500_ENABLED 1\n" + stubs + helper + r'''
int main(void) {
    assert(select_resolver() == 0);
    sta_connect = 1; assert(select_resolver() == 22);
    eth_connect = 1; assert(select_resolver() == 11);
    eth_connect = 0; assert(select_resolver() == 22);
    custom = 33; assert(select_resolver() == 33);
    custom = 99; assert(select_resolver() == 0);
    custom = 0; eth_connect = 1; eth.dns = 0;
    assert(select_resolver() == 0);
    puts("PASS: exit-node DNS uses leased Ethernet, WiFi failover, explicit resolver");
}
''')
    compile_run("dns_wifi", stubs + helper + r'''
int main(void) {
    eth_connect = 1; assert(select_resolver() == 0);
    sta_connect = 1; assert(select_resolver() == 22);
    puts("PASS: exit-node DNS in WiFi-only build");
}
''')


if __name__ == "__main__":
    test_routes()
    test_dns()
