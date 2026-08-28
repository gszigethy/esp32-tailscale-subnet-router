/* W5500 Ethernet uplink driver.
 *
 * Manages a WIZnet W5500 connected via SPI as the primary internet uplink.
 * ETH_EVENT and IP_EVENT_ETH_GOT_IP / IP_EVENT_ETH_LOST_IP are posted on
 * the default event loop; callers register handlers the same way as for
 * WiFi STA events.
 *
 * Call eth_uplink_init() once from app_main after esp_netif_init() and
 * esp_event_loop_create_default(). It returns the netif handle so the caller
 * can install it as the default route.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the W5500 Ethernet driver on the SPI pins configured via
 * Kconfig (CONFIG_ETH_W5500_* symbols). Creates the SPI bus, installs the
 * W5500 MAC/PHY driver, and attaches a DHCP-enabled Ethernet netif.
 *
 * Returns the netif handle on success, NULL on any init failure (hardware
 * absent, SPI conflict, etc.). On NULL, the firmware continues in WiFi-only
 * mode with no side-effects. */
esp_netif_t *eth_uplink_init(void);

/* True when the Ethernet link is up and a DHCP IP is active. */
bool eth_uplink_connected(void);

/* Fill *ip, *netmask, *gw with the current DHCP lease (network byte order).
 * Returns false and leaves outputs unchanged when disconnected. */
bool eth_uplink_get_ip_info(uint32_t *ip, uint32_t *netmask, uint32_t *gw);

/* MAC address of the W5500 (6 bytes). All-zero until the first
 * ETHERNET_EVENT_CONNECTED fires. */
void eth_uplink_get_mac(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
