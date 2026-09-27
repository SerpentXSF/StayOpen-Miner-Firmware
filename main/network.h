#ifndef NETWORK_H_
#define NETWORK_H_

#include "connect.h"

typedef struct
{
	char wifi_on;
    char wifi_ssid[32];
    char wifi_status[20];
    char wifi_ip_str[16]; // IP4ADDR_STRLEN_MAX
    bool wifi_ap_enabled;
	bool wifi_get_ip;

	char eth_on;
	char eth_status[20];
	char eth_ip_str[16];
	bool eth_get_ip;
}NetWorkInfo;

void network_set_wifi_status(wifi_status_t status, int retry_count, int reason);
void network_set_wifi_ap_status(bool enabled);

void network_eth_init_test(void);

void network_init(void * globalState);
NetWorkInfo  network_get_info(void);
bool network_is_connected(void);

/* Restart the Ethernet controller. Boards that power a hashboard off the
 * same supply can leave the W5500 wedged when that rail comes up. */
/* Bring up Ethernet that network_init() deferred until the hashboard was
 * powered. Returns ESP_ERR_INVALID_STATE when nothing was deferred. */
esp_err_t network_eth_start(void);

esp_err_t network_eth_recover(void);

/* Count one Ethernet driver failure. Called from the logging path, so it only
 * counts -- it must not log, allocate or block. */
void network_eth_note_driver_error(void);

/* Start watching the controller for mid-run failures. Safe to call on boards
 * with no Ethernet, and safe to call more than once. */
void network_eth_start_health_watch(void);

#endif /* NETWORK_H_ */
