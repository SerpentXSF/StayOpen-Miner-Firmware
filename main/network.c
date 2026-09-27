#include <stdio.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "mbedtls/platform.h"
#include "nvs_config.h"
#include "nvs_device.h"
#include "main.h"
#include "http_server.h"

#include "system.h"
#include "gpio_input_output.h"
#include "common.h"
#include "device.h"

#include "lv_input.h"
#include "network.h"
#include "ethernet_init.h"
#include "global_state.h"

static const char * TAG = "NETWORK";

static GlobalState * GLOBAL_STATE = NULL;

static NetWorkInfo netWork_Info;


void network_set_wifi_status(wifi_status_t status, int retry_count, int reason)
{
    switch(status) {
        case WIFI_CONNECTING:
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, 20, "Connecting...");
            return;
        case WIFI_CONNECTED:
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, 20, "Connected!");
			netWork_Info.wifi_get_ip = 1;
            if(netWork_Info.eth_on)
			{
				wifi_softap_off();
				ESP_LOGI(TAG, "Connected to SSID: %s", netWork_Info.wifi_ssid);
			}
            return;
        case WIFI_RETRYING:
            // See https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_wifi_types_generic.h for codes
            switch(reason) {
                case 201:
                    snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, 20, "No AP found (%d)", retry_count);
                    return;
                case 15:
                case 205:
                    snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, 20, "Password error (%d)", retry_count);
                    return;
                default:
                    snprintf(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, 20, "Error %d (%d)", reason, retry_count);
                    return;
            }
		case WIFI_DISCONNECTED:
			netWork_Info.wifi_get_ip = 0;
            if(retry_count)
            {
                if(netWork_Info.eth_get_ip == 0)
                {
                    ESP_LOGE(TAG, "Failed to connect to SSID: %s", netWork_Info.wifi_ssid);

                    strncpy(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, "Failed to connect", 20);
                    SYSTEM_notify_error_info(GLOBAL_STATE, WIFI_CONNETION_ERROR, NULL);
                    ESP_LOGI(TAG, "Finished, waiting for user input:");
                }
                else
                {
                    wifi_softap_off();
                    ESP_LOGI(TAG, "ap off , retry_count = %d", retry_count);
                    esp_log_level_set("wifi", CONFIG_LOG_DEFAULT_LEVEL_INFO);
                }
            }
			return;
    }
    ESP_LOGW(TAG, "Unknown status: %d", status);
}

void network_set_wifi_ap_status(bool enabled) 
{
    GLOBAL_STATE->SYSTEM_MODULE.ap_enabled = enabled;
}

int network_wifi_connect(void)
{
    int ret = -1;

    // pull the wifi credentials and hostname out of NVS
    char * wifi_ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, WIFI_SSID);
    char * wifi_pass = nvs_config_get_string(NVS_CONFIG_WIFI_PASS, WIFI_PASS);
    char * hostname  = nvs_config_get_string(NVS_CONFIG_HOSTNAME, HOSTNAME);

    strncpy(netWork_Info.wifi_ssid, wifi_ssid, sizeof(netWork_Info.wifi_ssid));
    netWork_Info.wifi_ssid[sizeof(netWork_Info.wifi_ssid)-1] = 0;

    int time_out = 0;
    if(!gpio_get_level(PIN_BUTTON_NEXT))
    {
        wifi_ssid[1] = 0;
        wifi_pass[1] = 0;
        time_out = 1;
    }

    // init and connect to wifi
    wifi_init(wifi_ssid, wifi_pass, hostname, netWork_Info.wifi_ip_str);

    if(time_out)
    {
        SYSTEM_notify_error_info(GLOBAL_STATE, WIFI_CONNETION_ERROR, NULL);
    }

	if(netWork_Info.eth_on == 0)
	{
	    // if the wifi is connected to ap, whether turn off the softap?
	    EventBits_t result_bits = wifi_connect();
	    wifi_sta_list_t sta_list;

	    if (result_bits & WIFI_CONNECTED_BIT) {
	        ESP_LOGI(TAG, "Connected to SSID: %s", wifi_ssid);
	        strncpy(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, "Connected!", 20);
	        SYSTEM_notify_status_change(GLOBAL_STATE, SYSTEM_WIFI_CONNECTED);

	        wifi_softap_off();
	    } else if (result_bits & WIFI_FAIL_BIT) {
	        ESP_LOGE(TAG, "Failed to connect to SSID: %s", wifi_ssid);

	        strncpy(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, "Failed to connect", 20);
	        SYSTEM_notify_error_info(GLOBAL_STATE, WIFI_CONNETION_ERROR, NULL);
	        /*set ap mode*/
	        /*wifi_only_softap();
	        ESP_LOGI(TAG, "wifi switch to soft ap mode for configuration.");*/
	        
	        // User might be trying to configure with AP, just chill here
	        ESP_LOGI(TAG, "Finished, waiting for user input:");
	        while (1) {
	            vTaskDelay(1000 / portTICK_PERIOD_MS);
	            if(esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK)
	            {
	                if (sta_list.num > 0)
	                {
	                    time_out = 1;
	                    ESP_LOGI(TAG, "AP Connect num: %d", sta_list.num);
	                }
	            }

	            //if(time_out)
	            {
	                time_out ++;
	                if(time_out > 90)
	                {
	                    restart_with_reason("WiFi connection timeout");
	                }
	            }
	        }
	    } else {
	        ESP_LOGE(TAG, "UNEXPECTED EVENT : %X",result_bits);
	        strncpy(GLOBAL_STATE->SYSTEM_MODULE.wifi_status, "unexpected error", 20);
	        // User might be trying to configure with AP, just chill here
	        SYSTEM_notify_error_info(GLOBAL_STATE, WIFI_CONNETION_ERROR, NULL);        
	        ESP_LOGI(TAG, "Finished, waiting for user input::");
	        while (1) {
	            vTaskDelay(1000 / portTICK_PERIOD_MS);
	            if(esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK)
	            {
	                if (sta_list.num > 0)
	                {
	                    time_out = 1;
	                    ESP_LOGI(TAG, "AP Connect num: %d", sta_list.num);
	                }
	            }
	            //if(time_out)
	            {
	                time_out ++;
	                if(time_out > 90)
	                {
	                    restart_with_reason("WiFi connection timeout");
	                }
	            }
	        }
	    }
	}

    if (NULL != wifi_ssid)
        free(wifi_ssid);
    if (NULL != wifi_pass)
        free(wifi_pass);
    if (NULL != hostname)
        free(hostname);
    
    return ret;
}

esp_err_t network_config_wifi_static_ip(void)
{
    esp_err_t ret = ESP_OK;
    uint16_t is_static_ip = nvs_config_get_u16(NVS_CONFIG_IS_STATIC_IP, 0);

    if(1 == is_static_ip){
        char* str_static_ip = nvs_config_get_string(NVS_CONFIG_STATIC_IP, "192.168.11.211");
        char* str_subnet_mask = nvs_config_get_string(NVS_CONFIG_SUBNET_MASK, "255.255.255.0");
        char* str_gateway = nvs_config_get_string(NVS_CONFIG_GATEWAY, "192.168.11.1");
        char* str_dns = nvs_config_get_string(NVS_CONFIG_DNS, "192.168.11.1");
    
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_dhcpc_stop(sta_netif);

        esp_netif_ip_info_t ip_info;
        ip4addr_aton(str_static_ip, (ip4_addr_t *)&ip_info.ip);
        ip4addr_aton(str_subnet_mask, (ip4_addr_t *)&ip_info.netmask);
        ip4addr_aton(str_gateway, (ip4_addr_t *)&ip_info.gw);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &ip_info));

        esp_netif_dns_info_t dns_info;
        ip4addr_aton(str_dns, (ip4_addr_t *)&dns_info.ip.u_addr.ip4);
        dns_info.ip.type = IPADDR_TYPE_V4;
        ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info));

        ESP_LOGI(TAG, "WIFI Static IP %s, mask %s, gw %s, dns %s", str_static_ip, str_subnet_mask, str_gateway, str_dns);

        if(NULL != str_static_ip)
            free(str_static_ip);
        if(NULL != str_subnet_mask)
            free(str_subnet_mask);
        if(NULL != str_gateway)
            free(str_gateway);
        if(NULL != str_dns)
            free(str_dns);
    }else{
        ESP_LOGI(TAG, "WIFI Dynamic IP");
    }

    return ret;
}

esp_err_t network_config_eth_static_ip(void)
{
    esp_err_t ret = ESP_OK;
    uint16_t is_static_ip = nvs_config_get_u16(NVS_CONFIG_ETH_IS_STATIC_IP, 0);

    if(1 == is_static_ip){
        char* str_static_ip = nvs_config_get_string(NVS_CONFIG_ETH_STATIC_IP, "192.168.2.222");
        char* str_subnet_mask = nvs_config_get_string(NVS_CONFIG_ETH_SUBNET_MASK, "255.255.255.0");
        char* str_gateway = nvs_config_get_string(NVS_CONFIG_ETH_GATEWAY, "192.168.2.1");
        char* str_dns = nvs_config_get_string(NVS_CONFIG_ETH_DNS, "192.168.2.1");
    
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        esp_netif_dhcpc_stop(sta_netif);

        esp_netif_ip_info_t ip_info;
        ip4addr_aton(str_static_ip, (ip4_addr_t *)&ip_info.ip);
        ip4addr_aton(str_subnet_mask, (ip4_addr_t *)&ip_info.netmask);
        ip4addr_aton(str_gateway, (ip4_addr_t *)&ip_info.gw);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &ip_info));

        esp_netif_dns_info_t dns_info;
        ip4addr_aton(str_dns, (ip4_addr_t *)&dns_info.ip.u_addr.ip4);
        dns_info.ip.type = IPADDR_TYPE_V4;
        ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info));

        ESP_LOGI(TAG, "ETH Static IP %s, mask %s, gw %s, dns %s", str_static_ip, str_subnet_mask, str_gateway, str_dns);

        if(NULL != str_static_ip)
            free(str_static_ip);
        if(NULL != str_subnet_mask)
            free(str_subnet_mask);
        if(NULL != str_gateway)
            free(str_gateway);
        if(NULL != str_dns)
            free(str_dns);
    }else{
        ESP_LOGI(TAG, "ETH Dynamic IP");
    }

    return ret;
}


/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
		snprintf(netWork_Info.eth_status, 20, "ETH Link Up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
		snprintf(netWork_Info.eth_status, 20, "ETH Link Down");
		netWork_Info.eth_get_ip = 0;
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
		snprintf(netWork_Info.eth_status, 20, "ETH Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
		snprintf(netWork_Info.eth_status, 20, "ETH Stopped");
		netWork_Info.eth_get_ip = 0;
        break;
    default:
        break;
    }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void eht_got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    #if 0
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    #endif
	snprintf(netWork_Info.eth_ip_str, IP4ADDR_STRLEN_MAX, IPSTR, IP2STR(&ip_info->ip));
	snprintf(netWork_Info.eth_status, 20, "ETH GetIP");
	netWork_Info.eth_get_ip = 1;
}

void network_eth_init_test(void)
{
    // Initialize Ethernet driver
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;

    /* Not every board in this family has the W5500. The BC01 is
     * WiFi-only, and aborting here took the whole device into a
     * boot loop rather than simply leaving Ethernet unavailable. */
    if (example_eth_init(&eth_handles, &eth_port_cnt) != ESP_OK || eth_port_cnt == 0) {
        ESP_LOGW(TAG, "No Ethernet controller found; continuing without it");
        netWork_Info.eth_on = 0;
        return;
    }

    // Initialize TCP/IP network interface aka the esp-netif (should be called only once in application)
    ESP_ERROR_CHECK(esp_netif_init());
    // Create default event loop that running in background
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Use ESP_NETIF_DEFAULT_ETH when just one Ethernet interface is used and you don't need to modify
    // default esp-netif configuration parameters.
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));

    // Register user defined event handers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eht_got_ip_event_handler, NULL));

    // Start Ethernet driver state machine
    for (int i = 0; i < eth_port_cnt; i++) {
        ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
    }
}

/* The controller this board is using, retained so it can be restarted. */
static esp_eth_handle_t s_eth_handle = NULL;

/* Set when network_init() chose to leave Ethernet until after the hashboard
 * is powered; claimed by whichever caller reaches network_eth_start() first. */
static bool eth_deferred = false;
static portMUX_TYPE eth_start_lock = portMUX_INITIALIZER_UNLOCKED;

/* How long the settle task waits for an address on either interface before
 * restarting the miner. 200 ms a tick, so five minutes -- the same window
 * network_init() used to enforce, moved to where it can still be satisfied. */
#define NET_SETTLE_TICKS (5 * 60 * 5)

/*
 * Runs once Ethernet has been started, and does the two things network_init()
 * can no longer do for itself now that it returns early on every deferred
 * path.
 *
 * First, the tidy-up from the end of its wait loop: drop the setup access
 * point, quieten the WiFi log, tell the system it is on the network. That had
 * simply stopped happening, so a miner that came up on the cable kept an open
 * access point broadcasting for as long as it was powered -- on images that
 * ship with a blank API password, which makes it an unauthenticated way in.
 *
 * Second, the backstop. network_init() restarted a miner that reached five
 * minutes with no address, and deferring Ethernet turned that into a loop:
 * the restart ran the same sequence and never got as far as the cable. The
 * backstop belongs here instead, after the interface that might supply an
 * address has actually been given the chance to. A miner that still has
 * nothing by then has a problem a restart might genuinely clear -- a DHCP
 * server that was not up yet, a switch port still learning, a driver that
 * came up wrong.
 *
 * A task rather than a loop in network_eth_start(), so main() is not held for
 * five minutes waiting on a board that may have no cable in it.
 */
static void network_settle_task(void *arg)
{
    wifi_sta_list_t sta_list;
    int idle = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (netWork_Info.eth_get_ip || netWork_Info.wifi_get_ip) {
            /* wifi_on guards the access point work: with WiFi switched off
             * there is no stack to talk to and nothing to tidy away. */
            if (netWork_Info.wifi_on && netWork_Info.eth_get_ip
                    && !netWork_Info.wifi_get_ip) {
                wifi_softap_off();
                ESP_LOGI(TAG, "eth get ip, wifi ap off");
                esp_log_level_set("wifi", CONFIG_LOG_DEFAULT_LEVEL_INFO);
            }
            SYSTEM_notify_status_change(GLOBAL_STATE, SYSTEM_WIFI_CONNECTED);
            ESP_LOGI(TAG, "Network settled after %d s", idle / 5);
            vTaskDelete(NULL);
        }

        /*
         * Somebody is connected to the setup access point, which is where a
         * miner with no network is configured from. Restarting out from
         * under them is the one thing this must not do, so the clock is held
         * at zero for as long as they are there.
         */
        if (netWork_Info.wifi_on
                && esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK
                && sta_list.num > 0) {
            idle = 0;
            continue;
        }

        if (++idle > NET_SETTLE_TICKS) {
            restart_with_reason("No address on either interface");
        }
    }
}

void network_eth_init(void)
{
	// Initialize Ethernet driver
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;

    /* Not every board in this family has the W5500. The BC01 is
     * WiFi-only, and aborting here took the whole device into a
     * boot loop rather than simply leaving Ethernet unavailable. */
    if (example_eth_init(&eth_handles, &eth_port_cnt) != ESP_OK || eth_port_cnt == 0) {
        ESP_LOGW(TAG, "No Ethernet controller found; continuing without it");
        netWork_Info.eth_on = 0;
        return;
    }

    // Initialize TCP/IP network interface aka the esp-netif (should be called only once in application)
    //ESP_ERROR_CHECK(esp_netif_init());
    // Create default event loop that running in background
    //ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Use ESP_NETIF_DEFAULT_ETH when just one Ethernet interface is used and you don't need to modify
    // default esp-netif configuration parameters.
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));

    // Register user defined event handers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eht_got_ip_event_handler, NULL));

    // Start Ethernet driver state machine
    for (int i = 0; i < eth_port_cnt; i++) {
        ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
    }

    /* Kept so the controller can be restarted later; see
     * network_eth_recover(). */
    s_eth_handle = eth_handles[0];
}

/*
 * Stop and restart the Ethernet controller.
 *
 * A BC04 leases an address, resolves DNS and syncs NTP happily, and then
 * roughly half a second after the core rail steps up to power the hashboard
 * every socket command starts timing out and never stops. The SPI reads and
 * writes themselves keep succeeding -- it is the W5500's command register
 * that stops clearing -- and nothing between those two events runs in
 * software at all: the failure lands inside a plain vTaskDelay, before the
 * ASICs have even been reset or clocked. That points at the inrush when the
 * rail comes up leaving the controller wedged.
 *
 * Nothing in the driver notices or retries, so the miner keeps an interface
 * that has an IP and cannot pass a packet. A stop/start re-runs the W5500's
 * own init and reopens its socket, which is the cheapest thing that can
 * clear that state.
 */
/*
 * Bring Ethernet up now, if network_init() deferred it.
 *
 * Called from main() once the hashboard is powered and settled, which is the
 * one ordering nothing has tested: whether a W5500 initialised after the
 * transient behaves, rather than one that was running through it.
 */
esp_err_t network_eth_start(void)
{
    /*
     * Two tasks can reach here now -- main(), once the hashboard is up, and
     * the stall watchdog, once it has concluded the hashboard never will be.
     * Claim the deferral under a lock so only one of them starts the driver;
     * initialising a W5500 twice adds the netif twice and panics.
     */
    bool claimed = false;

    portENTER_CRITICAL(&eth_start_lock);
    if (eth_deferred) {
        eth_deferred = false;
        claimed = true;
    }
    portEXIT_CRITICAL(&eth_start_lock);

    if (!claimed) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Two callers, and only one of them means "the hashboard is up" --
     * the stall watchdog means the opposite. Say the thing that is true
     * either way rather than asserting a state we are not checking. */
    ESP_LOGI(TAG, "Starting the deferred Ethernet controller now");
    network_eth_init();
    network_config_eth_static_ip();

    /*
     * From here the settle task owns the outcome: it tidies up when an
     * address arrives, and restarts the miner if none ever does.
     */
    if (pdPASS != xTaskCreate(network_settle_task, "net_settle", 4096, NULL,
                              3, NULL)) {
        ESP_LOGE(TAG, "Could not start the network settle task; the setup "
                      "access point will stay up and nothing will restart "
                      "this miner if it never gets an address");
    }

    return ESP_OK;
}

/*
 * Mid-run health for the W5500.
 *
 * network_eth_start() and network_eth_recover() both run once, from main(),
 * right after the hashboard comes up. That covers the transient this project
 * actually measured -- a BC04 whose socket commands all began timing out
 * ~500 ms after the core rail stepped up -- and nothing else. Once boot is
 * over nothing watches the controller at all, so an interface that wedges an
 * hour later keeps its IP, passes nothing, and says nothing.
 *
 * An owner's log prompted this: three lines at 16:55:13, mid-run, on a BC04
 * with Ethernet.
 *
 *   E w5500.mac: w5500_spi_write(142): spi transmit failed
 *   E w5500.mac: w5500_write_buffer(260): write TX buffer failed
 *   E w5500.mac: emac_w5500_transmit(652): write frame failed
 *
 * That one recovered by itself and needed nothing. The point is that if it had
 * not, this firmware would not have noticed.
 *
 * The driver reports these failures only by logging them, so the log hook is
 * the one place they are visible. It calls network_eth_note_driver_error(),
 * which does nothing but count -- it must not log, or allocate, or take a
 * lock, because it runs inside the logging path and anything it emits comes
 * straight back to it.
 *
 * The decision is left to this task instead. A single failed frame is noise:
 * one transmit failure produces three of these lines, and the owner's board
 * shrugged one off. A wedged controller produces them continuously, because
 * every retry fails the same way. So the trigger is a sustained burst, not an
 * event.
 */
#define ETH_ERR_WINDOW_MS      60000   /* how far back the count looks */
#define ETH_ERR_TRIGGER        30      /* ~10 failed frames in that window */
#define ETH_ERR_COOLDOWN_MS    600000  /* 10 min before trying again */
#define ETH_ERR_MAX_ATTEMPTS   3       /* then stop and leave it alone */

static volatile uint32_t s_eth_driver_errors = 0;

void network_eth_note_driver_error(void)
{
    /* Called from the logging path. Counts, and does nothing else. */
    s_eth_driver_errors++;
}

static void eth_health_task(void *arg)
{
    (void) arg;

    uint32_t window_start_count = s_eth_driver_errors;
    int64_t  window_start_us    = esp_timer_get_time();
    int64_t  last_recover_us    = 0;
    int      attempts           = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        int64_t now_us = esp_timer_get_time();
        uint32_t count = s_eth_driver_errors;
        uint32_t in_window = count - window_start_count;

        if ((now_us - window_start_us) >= (int64_t) ETH_ERR_WINDOW_MS * 1000) {
            /* Roll the window forward; a slow trickle must never accumulate
             * into a trigger. */
            window_start_count = count;
            window_start_us = now_us;
            continue;
        }

        if (in_window < ETH_ERR_TRIGGER) {
            continue;
        }

        if (attempts >= ETH_ERR_MAX_ATTEMPTS) {
            continue;   /* already said everything useful; see below */
        }

        if (last_recover_us != 0 &&
            (now_us - last_recover_us) < (int64_t) ETH_ERR_COOLDOWN_MS * 1000) {
            continue;
        }

        ESP_LOGE(TAG, "Ethernet controller reported %"PRIu32" driver errors in "
                      "under %d s -- it is not passing traffic. Restarting it.",
                 in_window, ETH_ERR_WINDOW_MS / 1000);

        attempts++;
        last_recover_us = now_us;
        window_start_count = s_eth_driver_errors;
        window_start_us = esp_timer_get_time();

        if (ESP_OK != network_eth_recover()) {
            /*
             * network_eth_recover() refuses to start a controller it could not
             * stop, because doing so panics the miner into a reboot loop. So a
             * failure here means Ethernet is down and staying down, and the
             * useful thing left is to say so plainly rather than keep trying.
             */
            ESP_LOGE(TAG, "could not restart Ethernet. It is down until this "
                          "miner is power cycled. WiFi, if configured, is "
                          "unaffected.");
        }

        if (attempts >= ETH_ERR_MAX_ATTEMPTS) {
            ESP_LOGE(TAG, "Ethernet has needed restarting %d times. Not trying "
                          "again -- something is wrong with the controller or "
                          "its supply, and repeated restarts will not fix it.",
                     attempts);
        }
    }
}

void network_eth_start_health_watch(void)
{
    static bool started = false;

    /* Only where there is a controller to watch. A BC01 has no W5500 and must
     * never grow a task that waits for one. */
    if (started || NULL == s_eth_handle) {
        return;
    }
    started = true;
    xTaskCreate(eth_health_task, "eth-health", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "watching the Ethernet controller for mid-run failures");
}

esp_err_t network_eth_recover(void)
{
    if (NULL == s_eth_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "Restarting the Ethernet controller after hashboard power-up");

    /*
     * The stop must be allowed to fail the whole operation. esp_eth_stop()
     * resets the PHY and waits for the link, which a W5500 that has stopped
     * answering will never report -- but its failure also means the driver
     * never emitted ETH_EVENT_STOP, so the netif is still attached and
     * running. Starting again from there re-enters esp_netif_action_start
     * and trips `assert failed: netif_add (netif already added)`, which
     * panics the miner into a reboot loop for as long as Ethernet is
     * enabled. Restarting a controller that cannot be stopped is not
     * something this can do.
     */
    esp_err_t ret = esp_eth_stop(s_eth_handle);
    if (ESP_OK != ret) {
        ESP_LOGE(TAG, "esp_eth_stop failed (%s); leaving Ethernet down",
                 esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    ret = esp_eth_start(s_eth_handle);
    if (ESP_OK != ret) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Ethernet controller restarted");
    }

    return ret;
}

void network_init(void * globalState)
{
	GLOBAL_STATE = (GlobalState *)globalState;
    wifi_sta_list_t sta_list;
    int time_out = 0;

	netWork_Info.wifi_on = (char)nvs_config_get_u16(NVS_CONFIG_WIFI_ON, 1);
	netWork_Info.eth_on = (char)nvs_config_get_u16(NVS_CONFIG_ETH_ON, 1);

	/*
	 * The BC01 family has no W5500, so there is nothing to defer and nothing
	 * to start. Say so from the board rather than trusting eth_on, which
	 * defaults to 1 and is absent from a published BC01 image -- one of those
	 * would otherwise take the deferred path, skip the wait for provisioning
	 * because it has no WiFi credentials either, and reach a code path that
	 * exists for a board it is not.
	 */
	if (netWork_Info.eth_on && device_is_bc01_family(GLOBAL_STATE->device_model)) {
		ESP_LOGI(TAG, "No Ethernet on this board; using WiFi only");
		netWork_Info.eth_on = 0;
	}

	ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    start_rest_server((void *) globalState);

	/*
	 * Ethernet is not started here. It is started after the hashboard is
	 * powered, by network_eth_start() from main().
	 *
	 * On a BC04 a W5500 that is already running when the core rail steps up
	 * to power the hashboard stops answering about 70 ms later and never
	 * recovers -- the interface keeps its address and passes nothing. One
	 * initialised *after* that transient is fine and stays fine, which is
	 * the whole of the fix: it is an ordering problem, not the hardware
	 * fault it looked like for a long time.
	 *
	 * Deferring it unconditionally matters most for the case that cannot
	 * fall back on WiFi. A freshly flashed miner has no WiFi credentials, so
	 * bringing Ethernet up early "because it is the only way in" would hand
	 * exactly those users the broken ordering. They get the setup access
	 * point to configure from, and a working Ethernet interface a few
	 * seconds later.
	 */
	if(netWork_Info.eth_on)
	{
		eth_deferred = true;
		ESP_LOGI(TAG, "Ethernet deferred until the hashboard is powered");
	}

	if(netWork_Info.wifi_on)
	{
		network_wifi_connect();
		network_config_wifi_static_ip();
		vTaskDelay(pdMS_TO_TICKS(200));
	}

	if((netWork_Info.wifi_on == 0) && (netWork_Info.eth_on == 0))
	{
		/* Neither interface asked for: Ethernet is the last resort, and
		 * there is no hashboard power-up to wait for that would help. */
		network_eth_init();
		network_config_eth_static_ip();
		vTaskDelay(pdMS_TO_TICKS(200));
	}

	/*
	 * Nothing below can produce an Ethernet address any more, so waiting for
	 * one would hang until the timeout restarted the miner. When Ethernet is
	 * deferred and WiFi cannot supply an address either -- no credentials,
	 * or switched off -- carry on and let main() power the board and start
	 * Ethernet. The setup access point stays up throughout, so a miner in
	 * that state is still reachable to be configured.
	 */
	if(eth_deferred &&
	   (netWork_Info.wifi_on == 0 || netWork_Info.wifi_ssid[0] == '\0'))
	{
		ESP_LOGI(TAG, "No WiFi to wait for; continuing so Ethernet can start "
		              "after the hashboard");
		return;
	}

	/*
	 * How long to sit here with no address. Deferring Ethernet made this a
	 * trap: the loop below waits for an address from either interface, but
	 * the Ethernet that could supply one has not been started yet and will
	 * not be until main() gets past this call. A miner with a good cable in
	 * it and an unreachable SSID therefore waits the full five minutes,
	 * restarts, and does the same thing again -- for ever, without ever
	 * trying the cable. Restarting cannot help, because the restart runs the
	 * identical sequence.
	 *
	 * So when Ethernet is deferred the wait is short and ends by returning
	 * rather than restarting: give WiFi long enough to finish DHCP if it is
	 * going to, then go and start the interface that is actually plugged in.
	 * WiFi keeps retrying in the background, so a slow or briefly absent
	 * access point still joins later.
	 */
	const int wait_ticks = eth_deferred ? (20 * 5) : (5 * 60 * 5);

	while(1)
	{
		vTaskDelay(pdMS_TO_TICKS(200));
		if(netWork_Info.eth_get_ip || netWork_Info.wifi_get_ip)
		{
            if((netWork_Info.eth_get_ip == 1)&&(netWork_Info.wifi_get_ip == 0))
            {
                wifi_softap_off();
                ESP_LOGI(TAG, "eth get ip, wifi ap off");
                esp_log_level_set("wifi", CONFIG_LOG_DEFAULT_LEVEL_INFO);
            }
			SYSTEM_notify_status_change(GLOBAL_STATE, SYSTEM_WIFI_CONNECTED);
			break;
		}

        if(esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK)
        {
            if (sta_list.num > 0)
            {
                time_out = 1;
            }
        }

        {
            time_out ++;
            if(time_out > wait_ticks)
            {
                if(eth_deferred)
                {
                    ESP_LOGW(TAG, "No address from WiFi after %d s; going on "
                                  "to start Ethernet after the hashboard",
                             wait_ticks / 5);
                    return;
                }
                restart_with_reason("Network configuration timeout");
            }
        }
    }
}

NetWorkInfo  network_get_info(void)
{
	return netWork_Info;
}

bool network_is_connected(void)
{
	return (netWork_Info.eth_get_ip | netWork_Info.wifi_get_ip);
}
