#include "ethernet_manager.h"

#include <stdio.h>
#include <string.h>
#include "app_defs.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "ethernet";

static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth_handle;
static volatile bool s_link_up;
static volatile bool s_has_ip;

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ETHERNET_EVENT_CONNECTED) {
        s_link_up = true;
        ESP_LOGI(TAG, "Link up");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        s_link_up = false;
        ESP_LOGW(TAG, "Link down");
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        s_has_ip = true;
        ESP_LOGI(TAG, "DHCP: ip " IPSTR ", mask " IPSTR ", gw " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw));
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        s_has_ip = false;
        ESP_LOGW(TAG, "IP lost");
    }
}

esp_err_t ethernet_manager_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();  // DHCP-клиент включён по умолчанию
    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "netif create");
    esp_netif_set_hostname(s_netif, ETH_HOSTNAME);

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL), TAG, "eth events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_handler, NULL), TAG, "ip events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, ip_event_handler, NULL), TAG, "ip events");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;   // 50 МГц от внешнего генератора на GPIO0
    emac_config.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_POWER_GPIO;   // как ETH.begin(..., power = 16) в Arduino

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_FAIL, TAG, "mac/phy create");

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle), TAG, "driver install");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth_handle)), TAG, "netif attach");
    return esp_eth_start(s_eth_handle);
}

bool ethernet_manager_is_ready(void)
{
    return s_link_up && s_has_ip;
}

void ethernet_manager_get_status(eth_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->link_up = s_link_up;
    out->has_ip = s_has_ip;
    if (!s_netif) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (s_has_ip && esp_netif_get_ip_info(s_netif, &ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip, out->ip, sizeof(out->ip));
        esp_ip4addr_ntoa(&ip_info.netmask, out->netmask, sizeof(out->netmask));
        esp_ip4addr_ntoa(&ip_info.gw, out->gateway, sizeof(out->gateway));
    }

    uint8_t mac[6];
    if (esp_netif_get_mac(s_netif, mac) == ESP_OK) {
        snprintf(out->mac, sizeof(out->mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}
