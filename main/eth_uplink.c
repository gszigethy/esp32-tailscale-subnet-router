/* W5500 Ethernet uplink driver.
 *
 * Initializes a WIZnet W5500 on SPI as the primary internet uplink.
 * Pin assignments default to the Seeed Studio XIAO W5500 Ethernet Adapter:
 *   CS=GPIO1  SCK=GPIO8  MISO=GPIO9  MOSI=GPIO10  INT=-1 (polling)
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
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "driver/spi_master.h"
#include "esp_netif.h"
#include "nvs_params.h"

static const char *TAG = "eth_uplink";

static bool     s_connected = false;
static uint32_t s_ip        = 0;
static uint32_t s_netmask   = 0;
static uint32_t s_gw        = 0;
static uint8_t  s_mac[6]    = {0};

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

    /* MAC + PHY (both integrated in the W5500) ------------------------------ */
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = -1;   /* no reset pin on the XIAO adapter */

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_w5500 failed — W5500 not detected");
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_w5500 failed");
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }

    /* Ethernet driver ------------------------------------------------------- */
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    err = esp_eth_driver_install(&eth_cfg, &eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install: %s", esp_err_to_name(err));
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }

    /* netif (DHCP client enabled by default) -------------------------------- */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    if (!netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        esp_eth_driver_uninstall(eth_handle);
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }

    /* Glue driver to netif */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    esp_netif_attach(netif, glue);

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

    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start: %s", esp_err_to_name(err));
        esp_netif_destroy(netif);
        esp_eth_driver_uninstall(eth_handle);
        spi_bus_free((spi_host_device_t)CONFIG_ETH_W5500_SPI_HOST);
        return NULL;
    }

    ESP_LOGI(TAG, "W5500 started  CS=%d SCK=%d MISO=%d MOSI=%d INT=%d  %dMHz",
             CONFIG_ETH_W5500_CS_GPIO, CONFIG_ETH_W5500_SCK_GPIO,
             CONFIG_ETH_W5500_MISO_GPIO, CONFIG_ETH_W5500_MOSI_GPIO,
             CONFIG_ETH_W5500_INT_GPIO, CONFIG_ETH_W5500_SPI_CLOCK_MHZ);
    return netif;
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
