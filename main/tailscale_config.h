/* Tailscale (microlink) settings and runtime state.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tailscale settings persisted in NVS under the existing esp32_nat namespace.
// Pointers reference internal storage; do not free or modify in place.
extern int32_t tailscale_enabled;        // 0=off, 1=on
extern char* tailscale_auth_key;         // tskey-auth-... (Tailscale) or hskey-auth-... (Headscale) preauth
extern char* tailscale_hostname;         // Hostname registered on the tailnet
extern char* tailscale_login_server;     // "" = Tailscale SaaS; otherwise Headscale URL (e.g. "http://192.168.1.42")
extern char* tailscale_ipn_version;      // Hostinfo.IPNVersion reported to the control plane; "" = not reported (default)
extern char* tailscale_advertise_routes; // Newline-separated CIDRs (e.g. "192.168.4.0/24\n192.168.1.0/24")
extern int32_t tailscale_advertise_ap;   // 1 (default) = the AP subnet is advertised as a subnet route, computed live from the AP settings; 0 = only the listed routes
extern int32_t tailscale_max_peers;      // Active WG tunnels (microlink default 16, range 1..64)
extern uint32_t tailscale_exit_node_ip;  // VPN IP (host byte order) of selected exit node; 0 = none
extern int32_t tailscale_netcheck_override;       // 1 = let netcheck override the chosen default region, 0 = always stay on default
extern int32_t tailscale_netcheck_threshold_ms;   // Switch only when measured RTT beats default-region RTT by at least this many ms
extern int32_t tailscale_default_derp_region;     // User-selected home DERP region id (0 = unset → Frankfurt fallback)
extern int32_t tailscale_lan_bypass;              // 1 = exit-node mode lets RFC1918 (private) traffic egress via STA directly; 0 = everything via tunnel
extern int32_t tailscale_accept_routes;           // 1 = honour subnet routes that peers advertise via AllowedIPs (matching dst → WG tunnel)
extern int32_t tailscale_snat_subnet_routes;      // 1 = SNAT (masquerade) tunnel→STA forwarded subnet-route/exit-node-server traffic to this device's STA IP; 0 = keep peer src (upstream needs a route back). Default 0.

// Accepted-routes table — rebuilt periodically from microlink peer info when
// tailscale_accept_routes=1. Each entry is a CIDR (network in host byte order).
// `tailscale_accepted_routes_count` is the number of valid entries (always
// authoritative — entries past it must not be read).
#define TAILSCALE_ACCEPTED_ROUTES_MAX 32
typedef struct {
    uint32_t network;     // host byte order
    uint8_t  prefix_len;  // CIDR prefix
} tailscale_accepted_route_t;
extern tailscale_accepted_route_t tailscale_accepted_routes[TAILSCALE_ACCEPTED_ROUTES_MAX];
extern int tailscale_accepted_routes_count;

// Runtime state
extern bool tailscale_connected;         // Tunnel is up and authenticated
extern uint32_t tailscale_tunnel_ip;     // Tailnet IP, network byte order (0 if not connected)

// Microlink handle accessor (opaque to callers that include this header).
// Returns NULL if Tailscale is disabled or microlink hasn't been initialised.
struct microlink_s;
struct microlink_s *tailscale_get_microlink(void);

// Lifecycle
void init_sntp_if_needed(void);          // Start SNTP once (idempotent); defined in tailscale_manager.c
void tailscale_init(void);               // Load NVS settings; call once from app_main
esp_err_t tailscale_connect(void);
void tailscale_disconnect(void);
bool tailscale_is_connected(void);
void tailscale_connect_task(void *pvParameters);

// Routing-decision helpers (called from netif hooks)
void tailscale_set_subnet(uint32_t ip, uint32_t mask);

/* Upstream's advertise_routes_effective() covered only the AP subnet plus
 * the manual list. This fork composes ETH and STA auto-routes as well, so
 * tailscale_compose_routes() below is the single source of truth and the
 * upstream helper is gone. */
bool tailscale_in_subnet(uint32_t ip);

// Compose the full advertised-routes string from all sources:
//   1. ts_routes — user-managed manual routes (Tailscale config page textarea)
//   2. eth_route_cidr — auto-detected Ethernet LAN CIDR (when eth_route_en=1 in NVS)
//   3. sta_route_cidr — auto-detected WiFi STA CIDR (when sta_route_en=1 in NVS)
//   4. AP subnet     — computed from NVS ap_ip/ap_mask (when tailscale_advertise_ap=1)
// All sources are deduplicated. Returns a malloc'd newline-separated string,
// or NULL when there is nothing to advertise. Caller must free().
char *tailscale_compose_routes(void);

#ifdef __cplusplus
}
#endif
