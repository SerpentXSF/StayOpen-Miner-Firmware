/*
 * Pool B: the second permanently-connected Stratum session for dual mining.
 *
 * This is not a failover for pool A. Both sessions stay up, and the single
 * ASIC's hashrate is time-sliced between them by the scheduler in
 * create_jobs_task -- so dual mining splits the hashrate, it does not add any.
 * Both pools must be SHA-256d.
 *
 * Ported from the SerpentX dual-pool work for BitAxe and NerdAxe, adapted to
 * this tree: connection setup mirrors stratum_task.c here rather than
 * upstream's stratum_socket helpers, and setup replies arrive as
 * STRATUM_RESULT_SETUP rather than being told apart by message id.
 *
 * It deliberately touches none of pool A's state. Its receive path uses the
 * reentrant STRATUM_V1_receive_jsonrpc_line_ctx with a private buffer, so it
 * cannot corrupt pool A's accumulator.
 */

#include <string.h>
#include <pthread.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "connect.h"
#include "network.h"
#include "global_state.h"
#include "stratum_api.h"
#include "stratum_poolb_task.h"
#include "stratum_recv_ctx.h"
#include "pool_scheduler.h"
#include "pool_failover.h"
#include "work_queue.h"

static const char *TAG = "stratum_poolb";

#define POOLB_CONNECT_TIMEOUT_MS 5000
#define POOLB_IDLE_DELAY_MS      2000
#define POOLB_RETRY_DELAY_MS     5000
#define POOLB_MAX_RETRIES        3

static void poolb_close(GlobalState *GLOBAL_STATE)
{
    /*
     * Clear the handle before closing it, under the same lock the share submit
     * path takes. asic_result_task can be writing a pool B share from another
     * task at any moment; if it read the handle while this freed it, it would
     * write into freed memory.
     */
    pthread_mutex_lock(&GLOBAL_STATE->transportB_lock);
    esp_transport_handle_t t = GLOBAL_STATE->transportB;
    GLOBAL_STATE->transportB = NULL;
    if (t != NULL) {
        esp_transport_close(t);
        esp_transport_destroy(t);
    }
    pthread_mutex_unlock(&GLOBAL_STATE->transportB_lock);

    GLOBAL_STATE->SYSTEM_MODULE.poolB_connected = false;
    /* queue_clear frees each mining_notify it drops */
    queue_clear(&GLOBAL_STATE->stratum_queueB);
}

void stratum_poolb_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;

    StratumApiV1Message poolb_message = {0};
    char *rxbuf = NULL;
    size_t rxsize = 0;

    /*
     * Pool B keeps its own failover, stepped independently of pool A's. Before
     * this, a pool B outage meant it retried a dead host forever and every
     * slice the scheduler gave it fell through to pool A -- the miner quietly
     * stopped dual mining and nothing said so.
     */
    pool_failover_t fo;
    pool_failover_init(&fo, POOLB_MAX_RETRIES,
                       module->poolB_fb_url != NULL && module->poolB_fb_url[0] != 0);

    ESP_LOGI(TAG, "pool B task started");

    while (1) {
        if (!GLOBAL_STATE->dual_enable || !GLOBAL_STATE->ASIC_initalized ||
            !network_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(POOLB_IDLE_DELAY_MS));
            continue;
        }

        int endpoint = pool_failover_endpoint(&fo);
        if (endpoint < 0) {
            /* both endpoints exhausted this cycle; pool B's share of the work
             * goes to pool A meanwhile, which beats idling the ASIC */
            pool_failover_step(&fo, PF_EV_DISCONNECTED);
            vTaskDelay(pdMS_TO_TICKS(POOLB_RETRY_DELAY_MS));
            continue;
        }

        bool use_fb = (endpoint == 1);
        module->poolB_is_using_failover = use_fb;

        char *url     = use_fb ? module->poolB_fb_url  : module->poolB_url;
        uint16_t port = use_fb ? module->poolB_fb_port : module->poolB_port;
        char *user    = use_fb ? module->poolB_fb_user : module->poolB_user;
        char *pass    = use_fb ? module->poolB_fb_pass : module->poolB_pass;
        if (url == NULL || url[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(POOLB_IDLE_DELAY_MS));
            continue;
        }

        tls_mode tls = (tls_mode)(use_fb ? module->poolB_fb_tls : module->poolB_tls);

        /*
         * Build and connect on a local handle. Publishing a half-open handle to
         * transportB would let the submit path write to a socket that never
         * came up.
         */
        esp_transport_handle_t transport = STRATUM_V1_transport_init(tls, module->poolB_cert);
        if (transport == NULL) {
            ESP_LOGE(TAG, "pool B transport init failed");
            pool_failover_step(&fo, PF_EV_DISCONNECTED);
            vTaskDelay(pdMS_TO_TICKS(POOLB_RETRY_DELAY_MS));
            continue;
        }
        if (tls != DISABLED) {
            esp_transport_ssl_set_common_name(transport, url);
        }

        ESP_LOGI(TAG, "pool B connecting to %s:%u", url, port);
        if (esp_transport_connect(transport, url, port, POOLB_CONNECT_TIMEOUT_MS) != ESP_OK) {
            ESP_LOGW(TAG, "pool B connect failed %s:%u", url, port);
            esp_transport_close(transport);
            esp_transport_destroy(transport);
            pool_failover_step(&fo, PF_EV_DISCONNECTED);
            vTaskDelay(pdMS_TO_TICKS(POOLB_RETRY_DELAY_MS));
            continue;
        }

        pthread_mutex_lock(&GLOBAL_STATE->transportB_lock);
        GLOBAL_STATE->transportB = transport;
        pthread_mutex_unlock(&GLOBAL_STATE->transportB_lock);
        module->poolB_connected = true;
        pool_failover_step(&fo, PF_EV_CONNECTED);
        ESP_LOGI(TAG, "pool B connected to %s:%u (%s)", url, port,
                 use_fb ? "failover" : "primary");

        /* a fresh accumulator per connection */
        if (rxbuf != NULL) {
            free(rxbuf);
            rxbuf = NULL;
            rxsize = 0;
        }

        GLOBAL_STATE->send_uidB = 1;
        STRATUM_V1_configure_version_rolling(GLOBAL_STATE->transportB,
                                             GLOBAL_STATE->send_uidB++,
                                             &GLOBAL_STATE->version_maskB);
        STRATUM_V1_subscribe(GLOBAL_STATE->transportB, GLOBAL_STATE->send_uidB++,
                             GLOBAL_STATE->asic_model_str);
        STRATUM_V1_authorize(GLOBAL_STATE->transportB, GLOBAL_STATE->send_uidB++,
                             user, pass);

        /*
         * Ask for extranonce updates, if the owner turned them on.
         *
         * The handler for MINING_SET_EXTRANONCE below has always been here,
         * so pool B could receive a rotation -- it simply never asked to. The
         * setting existed too: poolbxnsub is read out of NVS at startup into
         * poolB_extranonce_subscribe, and then nothing in the firmware ever
         * looked at that field. Pool A and the fallback both do this properly.
         *
         * Without it, pool B keeps whatever extranonce it was given when it
         * subscribed. Against a pool that rotates them the work goes stale and
         * the shares are refused, which reads as a pool problem rather than as
         * a request we never sent.
         *
         * Sent straight after authorize rather than waiting for its reply, as
         * pool A does: stratum is ordered over the connection, so the pool
         * still processes the authorize first.
         */
        if (GLOBAL_STATE->SYSTEM_MODULE.poolB_extranonce_subscribe) {
            STRATUM_V1_extranonce_subscribe(GLOBAL_STATE->transportB,
                                            GLOBAL_STATE->send_uidB++);
        }

        while (1) {
            if (!GLOBAL_STATE->dual_enable) {
                ESP_LOGI(TAG, "dual mining disabled, dropping pool B");
                poolb_close(GLOBAL_STATE);
                break;
            }

            char *line = STRATUM_V1_receive_jsonrpc_line_ctx(GLOBAL_STATE->transportB,
                                                             &rxbuf, &rxsize);
            if (line == NULL) {
                ESP_LOGW(TAG, "pool B connection lost, reconnecting");
                poolb_close(GLOBAL_STATE);
                pool_failover_step(&fo, PF_EV_DISCONNECTED);
                break;
            }

            STRATUM_V1_parse(&poolb_message, line);
            free(line);

            if (poolb_message.method == MINING_NOTIFY) {
                if (poolb_message.should_abandon_work) {
                    /*
                     * The mirror of cleanQueue(): pool B abandons its own work
                     * and leaves pool A's alone. Without this, pool B's stale
                     * jobs stayed valid and their nonces were submitted to a
                     * pool that had already moved on -- rejected as stale.
                     */
                    if (GLOBAL_STATE->stratum_queueB.count > 0) {
                        queue_clear(&GLOBAL_STATE->stratum_queueB);
                    }
                    for (uint32_t chain = 0; chain < MAX_CHAIN_NUM; chain++) {
                        if (!GLOBAL_STATE->chain_pluged[chain]) {
                            continue;
                        }
                        pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock[chain]);
                        ASIC_jobs_queue_clear_pool(&GLOBAL_STATE->ASIC_jobs_queue[chain], POOL_B);
                        for (int i = 0; i < 128; i++) {
                            if (GLOBAL_STATE->valid_jobs[chain] != NULL &&
                                GLOBAL_STATE->job_pool[chain][i] == POOL_B) {
                                GLOBAL_STATE->valid_jobs[chain][i] = 0;
                            }
                        }
                        pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain]);
                    }
                }
                if (GLOBAL_STATE->stratum_queueB.count == QUEUE_SIZE) {
                    mining_notify *stale =
                        (mining_notify *)queue_dequeue(&GLOBAL_STATE->stratum_queueB);
                    STRATUM_V1_free_mining_notify(stale);
                }
                poolb_message.mining_notification->difficulty = GLOBAL_STATE->stratum_difficultyB;
                queue_enqueue(&GLOBAL_STATE->stratum_queueB,
                              poolb_message.mining_notification);
                /* the queue owns it now */
                poolb_message.mining_notification = NULL;

            } else if (poolb_message.method == MINING_SET_DIFFICULTY) {
                if (poolb_message.new_difficulty != GLOBAL_STATE->stratum_difficultyB) {
                    GLOBAL_STATE->stratum_difficultyB = poolb_message.new_difficulty;
                    ESP_LOGI(TAG, "pool B difficulty: %lu",
                             (unsigned long)GLOBAL_STATE->stratum_difficultyB);
                }

            } else if (poolb_message.method == MINING_SET_VERSION_MASK ||
                       poolb_message.method == STRATUM_RESULT_VERSION_MASK) {
                GLOBAL_STATE->version_maskB = poolb_message.version_mask;

            } else if (poolb_message.method == STRATUM_RESULT_SUBSCRIBE ||
                       poolb_message.method == MINING_SET_EXTRANONCE) {
                if (poolb_message.extranonce_str != NULL &&
                    poolb_message.extranonce_2_len > 0) {
                    /*
                     * Swap under the lock create_jobs_task copies it under, so
                     * a job never reads the string while it is being freed.
                     */
                    pthread_mutex_lock(&GLOBAL_STATE->extranonceB_lock);
                    char *old = GLOBAL_STATE->extranonce_strB;
                    GLOBAL_STATE->extranonce_strB = strdup(poolb_message.extranonce_str);
                    GLOBAL_STATE->extranonce_2_lenB = poolb_message.extranonce_2_len;
                    pthread_mutex_unlock(&GLOBAL_STATE->extranonceB_lock);
                    free(old);
                    ESP_LOGI(TAG, "pool B extranonce %s, en2_len %d",
                             GLOBAL_STATE->extranonce_strB, GLOBAL_STATE->extranonce_2_lenB);
                }

            } else if (poolb_message.method == STRATUM_RESULT) {
                /* share acknowledgements; setup replies arrive as _SETUP */
                if (poolb_message.response_success) {
                    module->poolB_shares_accepted++;
                } else {
                    module->poolB_shares_rejected++;
                    ESP_LOGW(TAG, "pool B share rejected: %s",
                             poolb_message.error_str ? poolb_message.error_str : "unknown");
                }

            } else if (poolb_message.method == STRATUM_RESULT_SETUP) {
                if (!poolb_message.response_success) {
                    ESP_LOGE(TAG, "pool B setup rejected: %s",
                             poolb_message.error_str ? poolb_message.error_str : "unknown");
                }

            } else if (poolb_message.method == CLIENT_RECONNECT) {
                ESP_LOGW(TAG, "pool B requested reconnect");
                poolb_close(GLOBAL_STATE);
                pool_failover_step(&fo, PF_EV_DISCONNECTED);
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POOLB_RETRY_DELAY_MS));
    }

    vTaskDelete(NULL);
}
