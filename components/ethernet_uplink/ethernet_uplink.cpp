/*
 * This module brings up the board's wired Ethernet as the internet uplink
 * and reports link and address changes.
 */

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#include "ethernet_uplink.h"

// Above the Wi-Fi station (100), so the default route does not move to the
// camera network when it is joined.
#define ETH_UPLINK_ROUTE_PRIO 128

static const char *TAG = "trailcamhub_eth";

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    esp_eth_handle_t handle = *static_cast<esp_eth_handle_t *>(event_data);
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac[6] = {};
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
        esp_eth_ioctl(handle, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
        ESP_LOGI(TAG,
                 "Ethernet link up: %s Mbit/s, %s duplex, MAC "
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 speed == ETH_SPEED_100M ? "100" : "10",
                 duplex == ETH_DUPLEX_FULL ? "full" : "half", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down; check the cable");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started; waiting for link");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *event =
            static_cast<const ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG,
                 "Ethernet uplink ready; IP address: " IPSTR ", netmask: " IPSTR
                 ", gateway: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));
    } else if (event_id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "Ethernet uplink lost its IP address");
    }
}

esp_err_t ethernet_uplink_start(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_ETH_UPLINK_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_ETH_UPLINK_MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "Could not create the Ethernet MAC");
        return ESP_FAIL;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_ETH_UPLINK_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_ETH_UPLINK_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (phy == NULL) {
        mac->del(mac);
        ESP_LOGE(TAG, "Could not create the IP101 PHY driver");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle = NULL;
    esp_err_t error = esp_eth_driver_install(&eth_config, &handle);
    if (error != ESP_OK) {
        phy->del(phy);
        mac->del(mac);
        ESP_LOGE(TAG,
                 "Could not install the Ethernet driver (%s); check "
                 "the PHY address and reset GPIO",
                 esp_err_to_name(error));
        return error;
    }

    esp_netif_inherent_config_t netif_base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    netif_base.route_prio = ETH_UPLINK_ROUTE_PRIO;
    esp_netif_config_t netif_config = {};
    netif_config.base = &netif_base;
    netif_config.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;
    esp_netif_t *netif = esp_netif_new(&netif_config);
    if (netif == NULL) {
        ESP_LOGE(TAG, "Could not create the Ethernet network interface");
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(handle)),
                        TAG, "Could not attach Ethernet to its interface");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   eth_event_handler, NULL),
                        TAG, "Could not register Ethernet event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_ETH_GOT_IP,
                                                   ip_event_handler, NULL),
                        TAG, "Could not register IP event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_ETH_LOST_IP,
                                                   ip_event_handler, NULL),
                        TAG, "Could not register IP event handler");
    ESP_RETURN_ON_ERROR(esp_eth_start(handle), TAG, "Could not start Ethernet");
    return ESP_OK;
}
