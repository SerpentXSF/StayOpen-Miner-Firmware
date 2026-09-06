#include "esp_log.h"
// #include "addr_from_stdin.h"
#include "lt0051.h"
#include "connect.h"
#include "system.h"
#include "global_state.h"
#include "lwip/dns.h"
#include <lwip/tcpip.h>
#include "lwip/inet.h"
#include "nvs_config.h"
#include "stratum_task.h"
#include "work_queue.h"
#include "pool_scheduler.h"
#include "esp_wifi.h"
#include <esp_sntp.h>
#include <time.h>
#include "lvgl_porting.h"
#include "network.h"
#include <sys/time.h>
#include "esp_timer.h"
#include <stdbool.h>
#include "utils.h"
#include "main.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"

/* The payout address the shipped config templates carry, so an
 * unconfigured miner cannot quietly mine to somebody else's wallet.
 * Matched as a substring because the templates append a worker suffix. */
#define STRATUM_PLACEHOLDER_USER "REPLACE-WITH-YOUR"

#define PORT CONFIG_STRATUM_PORT
#define STRATUM_URL CONFIG_STRATUM_URL

#define FALLBACK_PORT CONFIG_FALLBACK_STRATUM_PORT
#define FALLBACK_STRATUM_URL CONFIG_FALLBACK_STRATUM_URL

#define STRATUM_PW CONFIG_STRATUM_PW
#define FALLBACK_STRATUM_PW CONFIG_FALLBACK_STRATUM_PW
#define STRATUM_DIFFICULTY CONFIG_STRATUM_DIFFICULTY

#define MAX_RETRY_ATTEMPTS 3
#define MAX_CRITICAL_RETRY_ATTEMPTS 5
#define MAX_EXTRANONCE_2_LEN 32

#define BUFFER_SIZE 1024

typedef struct
{
    uint32_t stratum_difficulty;
    bool isconnected;
    char lastResolvedIp[INET_ADDRSTRLEN];
} StratumTaskModule;

static const char * TAG = "stratum_task";

static StratumApiV1Message stratum_api_v1_message = {.error_str = NULL, .mining_notification = NULL};
StratumTaskModule STATUM_MODULE = {.stratum_difficulty = 8192, .isconnected = false, .lastResolvedIp = {0},};

static const char * primary_stratum_url;
static uint16_t primary_stratum_port;

struct timeval tcp_snd_timeout = {
    .tv_sec = 5,
    .tv_usec = 0
};

struct timeval tcp_rcv_timeout = {
    .tv_sec = 60 * 10,
    .tv_usec = 0
};

bool is_wifi_connected() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return true;
    } else {
        return false;
    }
}

void cleanQueue(GlobalState * GLOBAL_STATE) {
    ESP_LOGD(TAG, "Clean Jobs: clearing stratum queue");
    GLOBAL_STATE->abandon_work = 1;
    if(GLOBAL_STATE->stratum_queue.count > 0)
        queue_clear(&GLOBAL_STATE->stratum_queue);

    for(uint32_t chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++){
        if(GLOBAL_STATE->chain_pluged[chain_num]){
            pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

            /*
             * This is pool A's clean_jobs. It used to clear the whole ASIC
             * queue and invalidate all 128 slots -- including pool B's work,
             * which pool A has no business abandoning. Every nonce the ASIC
             * then returned for a pool B job was discarded as an unknown job,
             * on every new block. That is lost shares, and one of them could
             * be a block.
             */
            if (GLOBAL_STATE->ASIC_jobs_queue[chain_num].count > 0){
                ESP_LOGD(TAG, "Clean Jobs: clearing chain %"PRIu32" asic job queue", chain_num);
                if (GLOBAL_STATE->dual_enable) {
                    ASIC_jobs_queue_clear_pool(&GLOBAL_STATE->ASIC_jobs_queue[chain_num], POOL_A);
                } else {
                    ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue[chain_num]);
                }
            }

            for (int i = 0; i < 128; i++) {
                if(NULL == GLOBAL_STATE->valid_jobs[chain_num])
                    continue;
                if (GLOBAL_STATE->dual_enable &&
                    GLOBAL_STATE->job_pool[chain_num][i] == POOL_B) {
                    continue;   /* pool B's slot; not pool A's to invalidate */
                }
                GLOBAL_STATE->valid_jobs[chain_num][i] = 0;
            }

            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);
        }
    }
}

void stratum_reset_uid(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Resetting stratum uid");
    GLOBAL_STATE->send_uid = 1;
}

bool stratum_connected_status(void)
{
    return STATUM_MODULE.isconnected;
}

char * stratum_get_pool_ip(void)
{
    return STATUM_MODULE.lastResolvedIp;
}

bool stratum_last_resolved_hostname(char *host)
{
    struct hostent *primary_dns_addr = gethostbyname(host);
    if (primary_dns_addr == NULL) {
        ESP_LOGW(TAG, "Heartbeat. Failed DNS check for: %s!", host);
        return false;
    }

    inet_ntop(AF_INET, (void *)primary_dns_addr->h_addr_list[0], STATUM_MODULE.lastResolvedIp, INET_ADDRSTRLEN);

    return true;
}

void stratum_close_connection(GlobalState * GLOBAL_STATE)
{
    if (GLOBAL_STATE->transport == NULL) {
        ESP_LOGE(TAG, "Transport already NULL, not shutting down again..");
        return;
    }

    ESP_LOGE(TAG, "Shutting down transport and restarting...");
    esp_transport_close(GLOBAL_STATE->transport);
    esp_transport_destroy(GLOBAL_STATE->transport);
    GLOBAL_STATE->transport = NULL;
    cleanQueue(GLOBAL_STATE);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}

void stratum_primary_heartbeat(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    ESP_LOGI(TAG, "Starting heartbeat thread for primary pool: %s:%d", primary_stratum_url, primary_stratum_port);
    vTaskDelay(10000 / portTICK_PERIOD_MS);

    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    struct timeval tcp_timeout = {
        .tv_sec = 5,
        .tv_usec = 0
    };

    while (1)
    {
        if (GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback == false) {
            vTaskDelay(10000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGD(TAG, "Running Heartbeat on: %s!", primary_stratum_url);

        if (!is_wifi_connected()) {
            ESP_LOGD(TAG, "Heartbeat. Failed WiFi check!");
            vTaskDelay(10000 / portTICK_PERIOD_MS);
			if(!network_is_connected())
			{
				continue;
			}
        }

        if (!stratum_last_resolved_hostname(primary_stratum_url)) {
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGD(TAG, "Heartbeat. Connecting to %s:%d", STATUM_MODULE.lastResolvedIp, primary_stratum_port);

        esp_transport_handle_t hb_transport = STRATUM_V1_transport_init(DISABLED, NULL);
        if (hb_transport == NULL) {
            ESP_LOGD(TAG, "Heartbeat. Failed transport init!");
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            continue;
        }

        esp_err_t err = esp_transport_connect(hb_transport, STATUM_MODULE.lastResolvedIp, primary_stratum_port, 5000);
        if (err != ESP_OK)
        {
            ESP_LOGD(TAG, "Heartbeat. Failed connect check: %s:%d (errno %d)", STATUM_MODULE.lastResolvedIp, primary_stratum_port, err);
            esp_transport_close(hb_transport);
            esp_transport_destroy(hb_transport);
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            continue;
        }

        int hb_sock = esp_transport_get_socket(hb_transport);
        if (hb_sock >= 0) {
            if (setsockopt(hb_sock, SOL_SOCKET, SO_RCVTIMEO , &tcp_timeout, sizeof(tcp_timeout)) != 0) {
                ESP_LOGE(TAG, "Fail to setsockopt SO_RCVTIMEO ");
            }
        }

        int send_uid = 1;
        STRATUM_V1_subscribe(hb_transport, send_uid++, GLOBAL_STATE->asic_model_str);
        STRATUM_V1_authenticate(hb_transport, send_uid++, GLOBAL_STATE->SYSTEM_MODULE.pool_user, GLOBAL_STATE->SYSTEM_MODULE.pool_pass);

        static char recv_buffer[BUFFER_SIZE];
        memset(recv_buffer, 0, BUFFER_SIZE);
        int bytes_received = esp_transport_read(hb_transport, recv_buffer, BUFFER_SIZE - 1, 5000);

        esp_transport_close(hb_transport);
        esp_transport_destroy(hb_transport);

        if (bytes_received == -1)  {
            vTaskDelay(60000 / portTICK_PERIOD_MS);
            continue;
        }

        if (strstr(recv_buffer, "mining.notify") != NULL) {
            ESP_LOGI(TAG, "Heartbeat successful and in fallback mode. Switching back to primary.");
            GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback = false;
            stratum_close_connection(GLOBAL_STATE);
            continue;
        }

        vTaskDelay(60000 / portTICK_PERIOD_MS);
    }
}


void decode_mining_notification(GlobalState * GLOBAL_STATE, const mining_notify *mining_notification)
{
    double network_difficulty = networkDifficulty(mining_notification->target);
    suffixString(network_difficulty, GLOBAL_STATE->network_diff_string, DIFF_STRING_SIZE, 0);    

    int coinbase_1_len = strlen(mining_notification->coinbase_1) / 2;
    int coinbase_2_len = strlen(mining_notification->coinbase_2) / 2;
    
    int coinbase_1_offset = 41; // Skip version (4), inputcount (1), prevhash (32), vout (4)
    if (coinbase_1_len < coinbase_1_offset) return;

    uint8_t scriptsig_len;
    hex2bin(mining_notification->coinbase_1 + (coinbase_1_offset * 2), &scriptsig_len, 1);
    coinbase_1_offset++;

    if (coinbase_1_len < coinbase_1_offset) return;
    
    uint8_t block_height_len;
    hex2bin(mining_notification->coinbase_1 + (coinbase_1_offset * 2), &block_height_len, 1);
    coinbase_1_offset++;

    if (coinbase_1_len < coinbase_1_offset || block_height_len == 0 || block_height_len > 4) return;

    uint32_t block_height = 0;
    hex2bin(mining_notification->coinbase_1 + (coinbase_1_offset * 2), (uint8_t *)&block_height, block_height_len);
    coinbase_1_offset += block_height_len;

    if (block_height != GLOBAL_STATE->block_height) {
        ESP_LOGI(TAG, "Block height %d", block_height);
        GLOBAL_STATE->block_height = block_height;
    }

    size_t scriptsig_length = scriptsig_len - 1 - block_height_len - (strlen(GLOBAL_STATE->extranonce_str) / 2) - GLOBAL_STATE->extranonce_2_len;
    if (scriptsig_length <= 0) return;
    
    char * scriptsig = malloc(scriptsig_length + 1);

    int coinbase_1_tag_len = coinbase_1_len - coinbase_1_offset;
    hex2bin(mining_notification->coinbase_1 + (coinbase_1_offset * 2), (uint8_t *) scriptsig, coinbase_1_tag_len);

    int coinbase_2_tag_len = scriptsig_length - coinbase_1_tag_len;

    if (coinbase_2_len < coinbase_2_tag_len) return;
    
    if (coinbase_2_tag_len > 0) {
        hex2bin(mining_notification->coinbase_2, (uint8_t *) scriptsig + coinbase_1_tag_len, coinbase_2_tag_len);
    }

    for (int i = 0; i < scriptsig_length; i++) {
        if (!isprint((unsigned char)scriptsig[i])) {
            scriptsig[i] = '.';
        }
    }

    scriptsig[scriptsig_length] = '\0';

    if (GLOBAL_STATE->scriptsig == NULL || strcmp(scriptsig, GLOBAL_STATE->scriptsig) != 0) {
        ESP_LOGI(TAG, "Scriptsig: %s", scriptsig);

        char * previous_miner_tag = GLOBAL_STATE->scriptsig;
        GLOBAL_STATE->scriptsig = scriptsig;
        free(previous_miner_tag);
    } else {
        free(scriptsig);
    }
}

void stratum_task(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    primary_stratum_url = GLOBAL_STATE->SYSTEM_MODULE.pool_url;
    primary_stratum_port = GLOBAL_STATE->SYSTEM_MODULE.pool_port;
    char * stratum_url = GLOBAL_STATE->SYSTEM_MODULE.pool_url;
    uint16_t port = GLOBAL_STATE->SYSTEM_MODULE.pool_port;
    bool extranonce_subscribe = GLOBAL_STATE->SYSTEM_MODULE.pool_extranonce_subscribe;
    uint16_t difficulty = GLOBAL_STATE->SYSTEM_MODULE.pool_difficulty;

    STRATUM_V1_initialize_buffer();
    int retry_attempts = 0;
    int retry_critical_attempts = 0;

    //xTaskCreate(stratum_primary_heartbeat, "stratum primary heartbeat", 8192, pvParameters, 1, NULL);

    //ESP_LOGI(TAG, "Opening connection to pool: %s:%d", stratum_url, port);
    while (1) {
        if (!is_wifi_connected()) {
            //SYSTEM_notify_error_info(GLOBAL_STATE, WIFI_CONNETION_ERROR, NULL);
            ESP_LOGI(TAG, "WiFi disconnected, attempting to reconnect...");
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            if(!network_is_connected())
			{
                ESP_LOGI(TAG, "ETH disconnected, attempting to reconnect...");
                vTaskDelay(5000 / portTICK_PERIOD_MS);
				continue;
			}
        }

        if((strlen(GLOBAL_STATE->SYSTEM_MODULE.pool_url) < 5 ) && (strlen(GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url) < 5))
        {
            ESP_LOGI(TAG, "error pool : %s , %s ",GLOBAL_STATE->SYSTEM_MODULE.pool_url, GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url);
            vTaskDelay(10000 / portTICK_PERIOD_MS);
            continue;
        }

        if (retry_attempts >= MAX_RETRY_ATTEMPTS)
        {
            if (GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url == NULL || GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url[0] == '\0') {
                SYSTEM_notify_error_info(GLOBAL_STATE, NETWORK_ERROR, NULL);
                ESP_LOGI(TAG, "Unable to switch to fallback. No url configured. (retries: %d)...", retry_attempts);
                GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback = false;
                retry_attempts = 0;
                continue;
            }

            GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback = !GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback;
            // Reset share stats at failover
            for (int i = 0; i < GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats_count; i++) {
                GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].count = 0;
                GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].message[0] = '\0';
            }
            GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats_count = 0;
            GLOBAL_STATE->SYSTEM_MODULE.shares_accepted = 0;
            GLOBAL_STATE->SYSTEM_MODULE.shares_rejected = 0;
            GLOBAL_STATE->SYSTEM_MODULE.work_received = 0;
            GLOBAL_STATE->SYSTEM_MODULE.best_session_nonce_diff = 0;
            
            ESP_LOGI(TAG, "Switching target due to too many failures (retries: %d)...", retry_attempts);
            retry_attempts = 0;
        }

        stratum_url = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url : GLOBAL_STATE->SYSTEM_MODULE.pool_url;
        port = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_port : GLOBAL_STATE->SYSTEM_MODULE.pool_port;
        extranonce_subscribe = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_extranonce_subscribe : GLOBAL_STATE->SYSTEM_MODULE.pool_extranonce_subscribe;
        difficulty = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_difficulty : GLOBAL_STATE->SYSTEM_MODULE.pool_difficulty;

        ESP_LOGI(TAG, "Extranonce subscribe status: %s", extranonce_subscribe ? "ENABLED" : "DISABLED");

        if(retry_attempts == 0)
        {
            ESP_LOGI(TAG, "Opening connection to pool: %s:%d", stratum_url, port);
        }
        
        refresh_network_from_system(stratum_url, port);

        if (!stratum_last_resolved_hostname(stratum_url)) {
            ESP_LOGW(TAG, "hostname resolution failed for %s", stratum_url);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            retry_attempts++;
            continue;
        }

        tls_mode tls = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? \
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_tls : GLOBAL_STATE->SYSTEM_MODULE.pool_tls;
        char * cert = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? \
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_cert : GLOBAL_STATE->SYSTEM_MODULE.pool_cert;

        const char *protocol_prefix = (tls != DISABLED) ? "stratum+ssl" : "stratum+tcp";
        ESP_LOGI(TAG, "Connecting to: %s://%s:%d (%s)", protocol_prefix, stratum_url, port, STATUM_MODULE.lastResolvedIp);

        GLOBAL_STATE->transport = STRATUM_V1_transport_init(tls, cert);
        if (GLOBAL_STATE->transport == NULL) {
            ESP_LOGE(TAG, "Unable to create transport");
            if (++retry_critical_attempts > MAX_CRITICAL_RETRY_ATTEMPTS) {
                ESP_LOGE(TAG, "Max retry attempts reached, restarting...");
                restart_with_reason("Max transport creation retry attempts reached");
            }
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }
        retry_critical_attempts = 0;

        if (tls != DISABLED) {
            esp_transport_ssl_set_common_name(GLOBAL_STATE->transport, stratum_url);
        }

        ESP_LOGI(TAG, "Transport created, connecting to %s:%d", STATUM_MODULE.lastResolvedIp, port);
        esp_err_t err = esp_transport_connect(GLOBAL_STATE->transport, STATUM_MODULE.lastResolvedIp, port, 5000);
        if (err != ESP_OK)
        {
            retry_attempts++;
            ESP_LOGE(TAG, "Transport unable to connect to %s:%d (errno %d)", stratum_url, port, err);
            SYSTEM_notify_error_info(GLOBAL_STATE, NETWORK_ERROR, NULL);
            // close the transport
            esp_transport_close(GLOBAL_STATE->transport);
            esp_transport_destroy(GLOBAL_STATE->transport);
            GLOBAL_STATE->transport = NULL;
            // instead of restarting, retry this every 5 seconds
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        int sock = esp_transport_get_socket(GLOBAL_STATE->transport);
        if (sock >= 0) {
            if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tcp_snd_timeout, sizeof(tcp_snd_timeout)) != 0) {
                ESP_LOGE(TAG, "Fail to setsockopt SO_SNDTIMEO");
            }

            if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO , &tcp_rcv_timeout, sizeof(tcp_rcv_timeout)) != 0) {
                ESP_LOGE(TAG, "Fail to setsockopt SO_RCVTIMEO ");
            }
        }

        stratum_reset_uid(GLOBAL_STATE);
        cleanQueue(GLOBAL_STATE);

        ///// Start Stratum Action
        // mining.configure - ID: 1
        int configure_message_id = GLOBAL_STATE->send_uid++;
        STRATUM_V1_configure_version_rolling(GLOBAL_STATE->transport, configure_message_id, &GLOBAL_STATE->version_mask);

        // mining.subscribe - ID: 2
        int subscribe_message_id = GLOBAL_STATE->send_uid++;
        STRATUM_V1_subscribe(GLOBAL_STATE->transport, subscribe_message_id, GLOBAL_STATE->asic_model_str);

        char * username = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? \
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user : GLOBAL_STATE->SYSTEM_MODULE.pool_user;
        char * password = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? \
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_pass : GLOBAL_STATE->SYSTEM_MODULE.pool_pass;

        /*
         * The shipped config templates carry a placeholder payout address, so
         * that a miner flashed and left unconfigured cannot quietly mine to
         * somebody else's wallet. That is deliberate and worth keeping.
         *
         * What is not worth keeping is what the owner sees when they hit it.
         * The pool refuses the authorize and closes the connection, and the
         * only clue is "setup message rejected: 3,unknown" -- which says
         * nothing about the address. Observed on a BC04 that had been flashed
         * from the web flasher and never configured: a sixty-second connect,
         * reject, disconnect, retry loop with no indication why.
         */
        if (NULL != username && NULL != strstr(username, STRATUM_PLACEHOLDER_USER)) {
            ESP_LOGE(TAG, "=====================================================");
            ESP_LOGE(TAG, "The payout address is still the placeholder:");
            ESP_LOGE(TAG, "  %s", username);
            ESP_LOGE(TAG, "The pool will refuse this and close the connection.");
            ESP_LOGE(TAG, "Set your own address in Settings, or as stratumuser");
            ESP_LOGE(TAG, "in config.cvs. Nothing will mine until you do.");
            ESP_LOGE(TAG, "=====================================================");
        }

        int extranonce_message_id = -1;   /* set if we send one, below */
        int authorize_message_id = GLOBAL_STATE->send_uid++;
        // mining.authorize - ID: 3
        STRATUM_V1_authorize(GLOBAL_STATE->transport, authorize_message_id, username, password);

        // mining.suggest_difficulty - ID: 4
        // STRATUM_V1_suggest_difficulty(GLOBAL_STATE->sock, GLOBAL_STATE->send_uid++, STRATUM_DIFFICULTY);

        // Everything is set up, lets make sure we don't abandon work unnecessarily.
        GLOBAL_STATE->abandon_work = 0;

        while (1) {
            char * line = STRATUM_V1_receive_jsonrpc_line(GLOBAL_STATE->transport);
            if (!line) {
                SYSTEM_notify_error_info(GLOBAL_STATE, NETWORK_ERROR, NULL);
                ESP_LOGE(TAG, "Failed to receive JSON-RPC line, reconnecting...");
                retry_attempts++;
                stratum_close_connection(GLOBAL_STATE);
                break;
            }

            STATUM_MODULE.isconnected = true;

            ESP_LOGD(TAG, "rx: %s", line);
            STRATUM_V1_parse(&stratum_api_v1_message, line);
            free(line);

            #ifdef STATISTIC_SYSTEM_FEATURE
            SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;
            uint8_t pool_id = 0;
            if (module->is_using_fallback) {
                pool_id = 1;
            }
            #endif

            if (stratum_api_v1_message.method == MINING_NOTIFY) {
                GLOBAL_STATE->SYSTEM_MODULE.work_received++;
                SYSTEM_notify_new_ntime(GLOBAL_STATE, stratum_api_v1_message.mining_notification->ntime);
                if (stratum_api_v1_message.should_abandon_work &&
                    (GLOBAL_STATE->stratum_queue.count > 0 || GLOBAL_STATE->ASIC_jobs_queue[0].count > 0
                        || GLOBAL_STATE->ASIC_jobs_queue[1].count > 0)) {
                    cleanQueue(GLOBAL_STATE);
                }
                if (GLOBAL_STATE->stratum_queue.count == QUEUE_SIZE) {
                    mining_notify * next_notify_json_str = (mining_notify *) queue_dequeue(&GLOBAL_STATE->stratum_queue);
                    STRATUM_V1_free_mining_notify(next_notify_json_str);
                }
                stratum_api_v1_message.mining_notification->difficulty = STATUM_MODULE.stratum_difficulty;
                SYSTEM_notify_get_work(GLOBAL_STATE);

                #ifdef STATISTIC_SYSTEM_FEATURE
                statistic_notice_getwork(&(GLOBAL_STATE->STATISTIC_MODULE), pool_id);
                #endif

                queue_enqueue(&GLOBAL_STATE->stratum_queue, stratum_api_v1_message.mining_notification);
                //decode_mining_notification(GLOBAL_STATE, stratum_api_v1_message.mining_notification);
            } else if (stratum_api_v1_message.method == MINING_SET_DIFFICULTY) {
                if (stratum_api_v1_message.new_difficulty != STATUM_MODULE.stratum_difficulty) {
                    GLOBAL_STATE->stratum_difficulty = stratum_api_v1_message.new_difficulty;
                    STATUM_MODULE.stratum_difficulty = stratum_api_v1_message.new_difficulty;
                    ESP_LOGI(TAG, "Set stratum difficulty: %ld", STATUM_MODULE.stratum_difficulty);
                    #ifdef STATISTIC_SYSTEM_FEATURE
                    statistic_pool_set_diff(&(GLOBAL_STATE->STATISTIC_MODULE), pool_id, STATUM_MODULE.stratum_difficulty);
                    #endif
                }
                GLOBAL_STATE->new_set_mining_difficulty_msg = true;
            } else if (stratum_api_v1_message.method == MINING_SET_VERSION_MASK ||
                    stratum_api_v1_message.method == STRATUM_RESULT_VERSION_MASK) {
                // 1fffe000
                ESP_LOGD(TAG, "Set version mask: %08lx", stratum_api_v1_message.version_mask);
                GLOBAL_STATE->version_mask = stratum_api_v1_message.version_mask;
                GLOBAL_STATE->new_stratum_version_rolling_msg = true;
            } else if (stratum_api_v1_message.method == STRATUM_RESULT_SUBSCRIBE) {
                ESP_LOGI(TAG, "Initial Extranonce received - extranonce_str: %s, extranonce_2_len: %u", 
                    stratum_api_v1_message.extranonce_str, stratum_api_v1_message.extranonce_2_len);
                if(xSemaphoreTake(GLOBAL_STATE->global_parameter_mutex, pdMS_TO_TICKS(2000))){
                    if(NULL != GLOBAL_STATE->extranonce_str){
                        free(GLOBAL_STATE->extranonce_str);
                    }
                    GLOBAL_STATE->extranonce_str = strdup(stratum_api_v1_message.extranonce_str);
                    GLOBAL_STATE->extranonce_2_len = stratum_api_v1_message.extranonce_2_len;
                    xSemaphoreGive(GLOBAL_STATE->global_parameter_mutex);
                }else{
                    ESP_LOGE(TAG, "Failed to get the global_parameter_mutex.");
                }
            } else if(stratum_api_v1_message.method == MINING_SET_EXTRANONCE){
                ESP_LOGI(TAG, "Dynamic Extranonce updated - extranonce_str: %s, extranonce_2_len: %u",
                    stratum_api_v1_message.extranonce_str, stratum_api_v1_message.extranonce_2_len);
                if(xSemaphoreTake(GLOBAL_STATE->global_parameter_mutex, pdMS_TO_TICKS(2000))){
                    if(NULL != GLOBAL_STATE->extranonce_str){
                        free(GLOBAL_STATE->extranonce_str);
                    }
                    GLOBAL_STATE->extranonce_str = strdup(stratum_api_v1_message.extranonce_str);
                    GLOBAL_STATE->extranonce_2_len = stratum_api_v1_message.extranonce_2_len;
                    xSemaphoreGive(GLOBAL_STATE->global_parameter_mutex);
                }else{
                    ESP_LOGE(TAG, "Failed to get the global_parameter_mutex.");
                }
            }else if (stratum_api_v1_message.method == CLIENT_RECONNECT) {
                ESP_LOGE(TAG, "Pool requested client reconnect...");
                stratum_close_connection(GLOBAL_STATE);
                break;
            } else if (stratum_api_v1_message.method == STRATUM_RESULT) {
                if (stratum_api_v1_message.response_success) {
                    ESP_LOGD(TAG, "message result accepted");
                    SYSTEM_notify_accepted_share(GLOBAL_STATE);
                    #ifdef STATISTIC_SYSTEM_FEATURE
                    statistic_notice_share_accept(&(GLOBAL_STATE->STATISTIC_MODULE), pool_id);
                    #endif
                } else {
                    if(NULL != stratum_api_v1_message.error_str){
                        ESP_LOGW(TAG, "message result rejected: %lld,%s", stratum_api_v1_message.message_id, stratum_api_v1_message.error_str);
                        SYSTEM_notify_rejected_share(GLOBAL_STATE, stratum_api_v1_message.error_str);
                    } else {
                        ESP_LOGW(TAG, "message result rejected: %lld",stratum_api_v1_message.message_id);
                        SYSTEM_notify_rejected_share(GLOBAL_STATE, NULL);
                    }
                    #ifdef STATISTIC_SYSTEM_FEATURE
                    statistic_notice_share_reject(&(GLOBAL_STATE->STATISTIC_MODULE), pool_id);
                    #endif
                }
            } else if (stratum_api_v1_message.method == STRATUM_RESULT_SETUP) {
                // Reset retry attempts after successfully receiving data.
                retry_attempts = 0;
                if (stratum_api_v1_message.response_success) {
                    ESP_LOGI(TAG, "setup message accepted (id=%lld)", stratum_api_v1_message.message_id);
                    if (stratum_api_v1_message.message_id == authorize_message_id) {
                        // authorize 成功后发送 extranonce.subscribe（仅一次）
                        if (extranonce_subscribe) {
                            extranonce_message_id = GLOBAL_STATE->send_uid++;
                            STRATUM_V1_extranonce_subscribe(GLOBAL_STATE->transport, extranonce_message_id);
                        }
                    }
                } else {
                    /* Name the method. A bare id tells the owner nothing, and
                     * "3" is the one they most need to understand. */
                    const char *which = "unknown message";
                    if (stratum_api_v1_message.message_id == authorize_message_id) {
                        which = "mining.authorize -- the pool refused this "
                                "worker/payout address";
                    } else if (stratum_api_v1_message.message_id == subscribe_message_id) {
                        which = "mining.subscribe";
                    } else if (stratum_api_v1_message.message_id == configure_message_id) {
                        which = "mining.configure (version rolling)";
                    } else if (stratum_api_v1_message.message_id == extranonce_message_id) {
                        which = "mining.extranonce.subscribe";
                    }

                    /* The parser substitutes "unknown" when the pool sends
                     * error:null, which is exactly the case here. Printing it
                     * verbatim reads as though the pool said something. */
                    if(NULL != stratum_api_v1_message.error_str &&
                       0 != strcmp(stratum_api_v1_message.error_str, "unknown")) {
                        ESP_LOGE(TAG, "pool rejected %s (id %lld): %s",
                                 which, stratum_api_v1_message.message_id,
                                 stratum_api_v1_message.error_str);
                    } else {
                        ESP_LOGE(TAG, "pool rejected %s (id %lld), no reason given",
                                 which, stratum_api_v1_message.message_id);
                    }
                }
            }
        }
        STATUM_MODULE.isconnected = false;
    }
    vTaskDelete(NULL);
}
