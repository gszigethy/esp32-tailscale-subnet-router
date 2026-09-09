/* W5500 Ethernet uplink driver.
 *
 * Initializes a WIZnet W5500 on SPI as the primary internet uplink.
 * Pin assignments default to the Seeed Studio XIAO W5500 Ethernet Adapter
 * carried by a XIAO ESP32-S3. The adapter is labelled with XIAO "D" numbers,
 * which are NOT GPIO numbers on this module:
 *   D1/CS=GPIO2  D8/SCK=GPIO7  D9/MISO=GPIO8  D10/MOSI=GPIO9  INT=-1 (polling)
 * Override via Kconfig (CONFIG_ETH_W5500_*).
 *
 * This module only tracks hardware state (link up/down, DHCP IP/mask/GW).
 * Application-level responses — setting the default netif, spawning the
 * Tailscale task, copying DNS to the AP DHCP server — are handled by the
 * IP_EVENT_ETH_GOT_IP handler registered in main.c, mirroring how the WiFi
 * STA event handler works.
 *
 * SPDX-License-Identifier: MIT
 */

#include "eth_uplink.h"
#include <string.h>
#include <stdlib.h>   /* free() for the hostname string */
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "driver/spi_master.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_params.h"

static const char *TAG = "eth_uplink";

static bool     s_connected = false;
static uint32_t s_ip        = 0;
static uint32_t s_netmask   = 0;
static uint32_t s_gw        = 0;
static uint8_t  s_mac[6]    = {0};

/* Retained past eth_uplink_init() (unlike the local var it used to be) so
 * eth_uplink_set_enabled() can start/stop the same driver instance later
 * instead of tearing the whole thing down and rebuilding it. NULL when no
 * driver exists — hardware absent, Kconfig-disabled, or init failed. */
static esp_eth_handle_t s_eth_handle = NULL;

/* The master-switch desired state (not the live link — see
 * eth_uplink_connected() for that). Defaults true so a device upgrading
 * from a firmware without this switch keeps its existing always-on
 * behaviour; NVS overrides it once the operator has touched the toggle. */
static volatile bool s_uplink_en = true;

static void on_eth_event(void *arg, esp_event_base_t base,
                         int32_t id, void *data)
{
    esp_eth_handle_t hdl = *(esp_eth_handle_t *)data;
    switch (id) {
        case ETHERNET_EVENT_CONNECTED:
            esp_eth_ioctl(hdl, ETH_CMD_G_MAC_ADDR, s_mac);
            ESP_LOGI(TAG, "link up  MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     s_mac[0], s_mac[1], s_mac[2],
                     s_mac[3], s_mac[4], s_mac[5]);
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "link down");
            s_connected = false;
            s_ip = s_netmask = s_gw = 0;
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGD(TAG, "driver started");
            break;
        case ETHERNET_EVENT_STOP:
            s_connected = false;
            ESP_LOGD(TAG, "driver stopped");
            break;
        default:
            break;
    }
}

static void on_eth_got_ip(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    s_ip        = ev->ip_info.ip.addr;
    s_netmask   = ev->ip_info.netmask.addr;
    s_gw        = ev->ip_info.gw.addr;
    s_connected = true;
    ESP_LOGI(TAG, "DHCP  IP=" IPSTR "  mask=" IPSTR "  gw=" IPSTR,
             IP2STR(&ev->ip_info.ip),
             IP2STR(&ev->ip_info.netmask),
             IP2STR(&ev->ip_info.gw));
}

static void on_eth_lost_ip(void *arg, esp_event_base_t base,
                            int32_t id, void *data)
{
    ESP_LOGI(TAG, "IP lost");
    s_connected = false;
    s_ip = s_netmask = s_gw = 0;
}

esp_netif_t *eth_uplink_init(void)
{
    /* Read the master switch before touching any hardware, so the decision
     * below (start the driver, or leave it installed but stopped) is made
     * once, up front. Absent key = upgrading device or fresh NVS → stays at
     * the true default set above. */
    {
        uint8_t v = 1;
        if (nvs_param_get_u8("eth_uplink_en", &v) == ESP_OK) {
            s_uplink_en = (v != 0);
        }
    }

    /* SPI bus ---------------------------------------------------------------- */
    spi_bus_config_t buscfg = {
        .miso_io_num   = CONFIG_ETH_W5500_MISO_GPIO,
        .mosi_io_num   = CONFIG_ETH_W5500_MOSI_GPIO,
        .sclk_io_num   = CONFIG_ETH_W5500_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST,
                                       &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return NULL;
    }

    /* W5500 SPI device ------------------------------------------------------ */
    spi_device_interface_config_t devcfg = {
        .command_bits   = 16,   /* W5500 frame: 16-bit address field  */
        .address_bits   = 8,    /* W5500 frame: 8-bit  control field  */
        .mode           = 0,
        .clock_speed_hz = CONFIG_ETH_W5500_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num   = CONFIG_ETH_W5500_CS_GPIO,
        .queue_size     = 20,
    };

    eth_w5500_config_t w5500cfg = ETH_W5500_DEFAULT_CONFIG(
        (spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST, &devcfg);
    w5500cfg.int_gpio_num = CONFIG_ETH_W5500_INT_GPIO;
    /* esp_eth_mac_new_w5500 requires exactly one of interrupt or polling
     * mode: (int_gpio_num >= 0) XOR (poll_period_ms > 0). Leaving the
     * default poll_period_ms of 0 with INT at -1 satisfies neither and the
     * config is rejected outright, so supply a period when INT is unwired. */
    if (CONFIG_ETH_W5500_INT_GPIO < 0) {
        w5500cfg.poll_period_ms = 10;
    }

    /* MAC + PHY (both integrated in the W5500) ------------------------------ */
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = -1;   /* no reset pin on the XIAO adapter */

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_w5500 failed — bad config or out of memory");
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_w5500 failed");
        mac->del(mac);
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }

    /* Ethernet driver ------------------------------------------------------- */
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    err = esp_eth_driver_install(&eth_cfg, &eth_handle);
    if (err == ESP_OK) {
        /* The W5500 has no factory-burned MAC and neither the driver nor
         * esp_eth_driver_install assigns one — the SHAR register keeps its
         * all-zero reset value unless we write it. With a zero source MAC
         * the PHY still links (the switch shows the port up) but every frame
         * we send is dropped as invalid, so DHCP never completes and the
         * device is unreachable. Push the ESP's designated Ethernet MAC in
         * before esp_netif_attach, which copies it onto the netif. */
        uint8_t eth_mac[6] = {0};
        esp_err_t merr = esp_read_mac(eth_mac, ESP_MAC_ETH);
        if (merr == ESP_OK) {
            merr = esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);
        }
        if (merr != ESP_OK) {
            ESP_LOGE(TAG, "set MAC address failed: %s", esp_err_to_name(merr));
        } else {
            /* Seed the status cache here rather than waiting for
             * ETHERNET_EVENT_CONNECTED: the /api/status ETH card reads this,
             * and with an unplugged cable it would otherwise render
             * 00:00:00:00:00:00 — the exact symptom of the MAC bug this code
             * exists to fix, which makes diagnosing an unplugged cable
             * needlessly confusing. */
            memcpy(s_mac, eth_mac, sizeof s_mac);
            ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     eth_mac[0], eth_mac[1], eth_mac[2],
                     eth_mac[3], eth_mac[4], eth_mac[5]);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install: %s", esp_err_to_name(err));
        /* The MAC owns the SPI device handle, so it must be deleted before
         * the bus — otherwise spi_bus_free reports "not all CSses freed"
         * and the device leaks. Same for every path below. */
        phy->del(phy);
        mac->del(mac);
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }

    /* netif (DHCP client enabled by default) -------------------------------- */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    if (!netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        esp_eth_driver_uninstall(eth_handle);
        phy->del(phy);
        mac->del(mac);
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }

    /* Glue driver to netif. Both calls must be checked: without the glue
     * attached the netif never sees ETHERNET_EVENT_CONNECTED, so it stays
     * down and the DHCP client never starts — while esp_eth_start() below
     * still succeeds. That failure mode looks exactly like a dead cable. */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    if (!glue) {
        ESP_LOGE(TAG, "esp_eth_new_netif_glue failed");
        esp_netif_destroy(netif);
        esp_eth_driver_uninstall(eth_handle);
        phy->del(phy);
        mac->del(mac);
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }
    err = esp_netif_attach(netif, glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach: %s", esp_err_to_name(err));
        esp_eth_del_netif_glue(glue);
        esp_netif_destroy(netif);
        esp_eth_driver_uninstall(eth_handle);
        phy->del(phy);
        mac->del(mac);
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }

    /* Propagate device-wide hostname into DHCP Option 12 */
    char *hostname = nvs_param_get_str("hostname");
    if (hostname && hostname[0]) {
        esp_netif_set_hostname(netif, hostname);
        ESP_LOGI(TAG, "hostname → '%s'", hostname);
    }
    free(hostname);

    /* Internal event handlers — update s_connected / s_ip for status queries */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               on_eth_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                               on_eth_lost_ip, NULL));

    /* Only start the driver if the master switch is on. Skipping esp_eth_start()
     * — rather than skipping everything above and returning NULL — leaves the
     * driver fully installed and glued to the netif, so eth_uplink_set_enabled()
     * can start it later with a plain esp_eth_start() call, no reboot needed.
     * Mirrors how wifi_init_sta() leaves the STA netif created but installs no
     * credentials while sta_uplink_en is off. */
    if (s_uplink_en) {
        err = esp_eth_start(eth_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_eth_start: %s", esp_err_to_name(err));
            /* Unwind in the reverse order of construction, glue included — the
             * attach above bound it to the netif, and destroying the netif with
             * the glue still registered leaks it and leaves the driver holding a
             * pointer to freed netif state. */
            esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event);
            esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_eth_got_ip);
            esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_eth_lost_ip);
            esp_eth_del_netif_glue(glue);
            esp_netif_destroy(netif);
            esp_eth_driver_uninstall(eth_handle);
            phy->del(phy);
            mac->del(mac);
            spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
            return NULL;
        }
        ESP_LOGI(TAG, "W5500 started  CS=%d SCK=%d MISO=%d MOSI=%d INT=%d  %dMHz",
                 CONFIG_ETH_W5500_CS_GPIO, CONFIG_ETH_W5500_SCK_GPIO,
                 CONFIG_ETH_W5500_MISO_GPIO, CONFIG_ETH_W5500_MOSI_GPIO,
                 CONFIG_ETH_W5500_INT_GPIO, CONFIG_ETH_W5500_SPI_CLOCK_MHZ);
    } else {
        ESP_LOGI(TAG, "W5500 uplink disabled (eth_uplink_en=0) — driver ready, not started");
    }

    s_eth_handle = eth_handle;
    return netif;
}

void eth_uplink_set_enabled(uint8_t enable)
{
    bool want = (enable != 0);
    s_uplink_en = want;
    nvs_param_set_u8("eth_uplink_en", want ? 1 : 0);

    if (!s_eth_handle) {
        /* No driver to drive — hardware absent, Kconfig-disabled, or init
         * failed. The switch still persists so the choice sticks if hardware
         * shows up later (e.g. after a Kconfig rebuild). */
        ESP_LOGW(TAG, "uplink %s — no W5500 driver present, nothing to start/stop",
                 want ? "enabled" : "disabled");
        return;
    }

    if (want) {
        esp_err_t err = esp_eth_start(s_eth_handle);
        /* INVALID_STATE here just means "already started" (esp_eth_start's
         * own FSM guard) — not an error from this switch's point of view. */
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "uplink enabled — driver started");
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        }
    } else {
        esp_err_t err = esp_eth_stop(s_eth_handle);
        /* esp_eth_stop() posts ETHERNET_EVENT_STOP, which the netif glue turns
         * into esp_netif_action_stop() — that brings the netif down, stops its
         * DHCP client and clears its IP automatically, and our own on_eth_event
         * handler clears s_connected on the same event. No manual state
         * cleanup needed here. */
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "uplink disabled — driver stopped");
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_eth_stop failed: %s", esp_err_to_name(err));
        }
    }
}

bool eth_uplink_is_enabled(void)
{
    return s_uplink_en;
}

bool eth_uplink_connected(void)
{
    return s_connected;
}

bool eth_uplink_get_ip_info(uint32_t *ip, uint32_t *netmask, uint32_t *gw)
{
    if (!s_connected) return false;
    if (ip)      *ip      = s_ip;
    if (netmask) *netmask = s_netmask;
    if (gw)      *gw      = s_gw;
    return true;
}

void eth_uplink_get_mac(uint8_t mac[6])
{
    memcpy(mac, s_mac, 6);
}
