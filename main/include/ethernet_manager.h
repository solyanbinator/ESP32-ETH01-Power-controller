#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool link_up;
    bool has_ip;
    char ip[16];
    char netmask[16];
    char gateway[16];
    char mac[18];
} eth_status_t;

// Запускает Ethernet (LAN8720), IP получается по DHCP.
esp_err_t ethernet_manager_start(void);

// Есть линк и IP-адрес — можно обращаться к серверу.
bool ethernet_manager_is_ready(void);

void ethernet_manager_get_status(eth_status_t *out);
