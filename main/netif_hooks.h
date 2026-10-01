/* lwIP netif hook installer — currently just the ACL packet filter.
 *
 * Wires acl_check_packet into the AP netif and both uplink netifs (WiFi STA
 * and wired ETH) input + linkoutput chains so the four ACL chains
 * (to_esp / from_esp / to_ap / from_ap) actually drop denied traffic. The two
 * uplinks share the to_esp/from_esp chains, matching how acl.h defines them
 * ("uplink input" / "uplink output"). Idempotent; call once after WiFi has
 * started AND after eth_uplink_init(), so every netif exists.
 *
 * Future hooks (byte counters, TTL override, kill switch)
 * land here too, sharing the same install/save-original-fn pattern.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void netif_hooks_init(void);

/* Wire-byte counters — accumulated in the four hook tap points before
 * the ACL check, so they represent everything that actually hit the
 * interface (denied frames included). Read-only from the application. */
uint64_t netif_hooks_get_sta_bytes_in (void);
uint64_t netif_hooks_get_sta_bytes_out(void);
uint64_t netif_hooks_get_ap_bytes_in  (void);
uint64_t netif_hooks_get_ap_bytes_out (void);
uint64_t netif_hooks_get_eth_bytes_in (void);
uint64_t netif_hooks_get_eth_bytes_out(void);

/* TTL hop-limit override on the uplink (applies to WiFi STA and wired ETH
 * alike; the name is kept for the NVS key and API compatibility). When
 * non-zero, every outgoing IPv4 frame on an uplink gets its TTL rewritten
 * to this value (with an RFC 1624 incremental checksum update) before
 * leaving the radio. 0 disables the override — the IP stack's natural
 * 64-default TTL passes through unchanged. Typical use: cloak a tethered
 * device behind the phone's own TTL (~64) so the upstream carrier
 * can't distinguish "phone" packets (TTL=64) from "tethered laptop"
 * packets (TTL=63 after one hop) and apply hotspot-throttle rules. */
void    netif_hooks_set_sta_ttl(uint8_t ttl);
uint8_t netif_hooks_get_sta_ttl(void);

#ifdef __cplusplus
}
#endif
