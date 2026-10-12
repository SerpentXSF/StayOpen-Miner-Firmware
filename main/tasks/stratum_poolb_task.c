/*
 * Every Stratum session that is not pool A's, in one task.
 *
 * These are not failovers for pool A. All of them stay up, and the single
 * ASIC chain's hashrate is time-sliced between them by the scheduler in
 * create_jobs_task -- so more pools split the hashrate, they never add any.
 * All of them must be SHA-256d.
 *
 * One task, not one per pool, and that is the whole reason quad pool fits on
 * this hardware. The two Stratum tasks were already the fattest things on a
 * BC04 -- measured at 6992 and 6936 bytes of stack used out of 8192 -- on a
 * board whose internal RAM has been seen down to double digits. Four of them
 * would want 32 KB of internal RAM that does not exist. Holding three
 * sessions in one task costs one stack instead of three.
 *
 * What makes that possible was already here: each session owns its receive
 * accumulator rather than sharing one, so sessions cannot corrupt each
 * other's partial lines. The missing piece was a receive that gives up
 * instead of blocking, which is STRATUM_V1_poll_jsonrpc_line_ctx.
 *
 * Ported from the SerpentX dual-pool work for BitAxe and NerdAxe, adapted to
 * this tree: connection setup mirrors stratum_task.c here rather than
 * upstream's stratum_socket helpers, and setup replies arrive as
 * STRATUM_RESULT_SETUP rather than being told apart by message id.
 *
 * It deliberately touches none of pool A's state.
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
#include "asic.h"
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

/*
 * How long a sweep waits on each connected pool before moving to the next.
 *
 * The task's responsiveness is this times the number of connected pools: 100
 * ms with one, 300 ms with three. Nothing here is latency-critical -- jobs
 * arrive every half minute or so, and shares are submitted by
 * asic_result_task writing to the transport directly, not through this loop
 * -- so a third of a second costs nothing and keeps the task off the CPU.
 */
#define POOL_RX_SLICE_MS 100

/*
 * Quad pool is for boards with four ASICs.
 *
 * A product decision, not a technical one, and worth saying so plainly: work
 * is time-sliced across the whole chain rather than aimed at a chip, so the
 * number of pools and the number of ASICs are independent -- a one-ASIC board
 * would divide its hashrate four ways perfectly happily. Pools beyond B are
 * ignored below this count because that is how the feature is meant to ship.
 */
#define QUAD_POOL_MIN_ASICS 4

typedef struct {
    pool_failover_t fo;
    char *          rxbuf;
    size_t          rxsize;
    int64_t         next_attempt_us;  /* do not redial before this */
    bool            fo_ready;
} secondary_state_t;

/*
 * File scope, not on the stack: this task's stack is the tightest on the
 * board and three of these would be three more things competing for it.
 */
static secondary_state_t s_state[POOL_MAX];

static const char *pool_name(pool_id_t id)
{
    switch (id) {
        case POOL_B: return "pool B";
        case POOL_C: return "pool C";
        case POOL_D: return "pool D";
        default:     return "pool A";
    }
}

/* A pool counts as configured once it has somewhere to connect to. */
static bool pool_configured(SystemModule *module, pool_id_t id)
{
    const char *url = module->pool[id].url;
    return url != NULL && url[0] != '\0';
}

static bool pool_allowed(GlobalState *GLOBAL_STATE, pool_id_t id)
{
    if (id == POOL_B) {
        return true;
    }
    return ASIC_get_asic_count(GLOBAL_STATE) >= QUAD_POOL_MIN_ASICS;
}

static void pool_abandon_work(GlobalState *GLOBAL_STATE, pool_id_t id);

static void pool_close(GlobalState *GLOBAL_STATE, pool_id_t id)
{
    /*
     * Clear the handle before closing it, under the same lock the share submit
     * path takes. asic_result_task can be writing a share from another task at
     * any moment; if it read the handle while this freed it, it would write
     * into freed memory.
     */
    pthread_mutex_lock(&GLOBAL_STATE->pool_session[id].transport_lock);
    esp_transport_handle_t t = GLOBAL_STATE->pool_session[id].transport;
    GLOBAL_STATE->pool_session[id].transport = NULL;
    if (t != NULL) {
        esp_transport_close(t);
        esp_transport_destroy(t);
    }
    pthread_mutex_unlock(&GLOBAL_STATE->pool_session[id].transport_lock);

    GLOBAL_STATE->SYSTEM_MODULE.pool[id].connected = false;
    /* queue_clear frees each mining_notify it drops */
    queue_clear(&GLOBAL_STATE->pool_session[id].queue);

    /*
     * Everything that belonged to the session that just ended goes with it.
     *
     * Without this, a reconnect inherits the dead session's work. The jobs
     * still sitting in the ASIC queue and in valid_jobs carry the old job
     * ids, so every nonce they return is submitted on the new socket against
     * a job the pool has never heard of and refused -- counted against this
     * pool as a rejected share, which reads as the pool being difficult
     * rather than as us sending it nonsense.
     */
    pool_abandon_work(GLOBAL_STATE, id);

    /*
     * And the extranonce, which is per-session: most pools hand out a new
     * extranonce1 on every subscribe. Leaving the old one set means
     * generate_work() finds it non-empty and builds work from it the moment
     * the new transport is published -- before the new subscribe has even
     * been answered -- so the first jobs of every reconnect are built on the
     * previous session's extranonce and rejected.
     */
    pthread_mutex_lock(&GLOBAL_STATE->pool_session[id].extranonce_lock);
    free(GLOBAL_STATE->pool_session[id].extranonce_str);
    GLOBAL_STATE->pool_session[id].extranonce_str = NULL;
    GLOBAL_STATE->pool_session[id].extranonce_2_len = 0;
    pthread_mutex_unlock(&GLOBAL_STATE->pool_session[id].extranonce_lock);

    if (s_state[id].rxbuf != NULL) {
        free(s_state[id].rxbuf);
        s_state[id].rxbuf = NULL;
        s_state[id].rxsize = 0;
    }
}

/*
 * One connection attempt. Returns true if the session came up.
 *
 * Blocking, and deliberately only ever called for one pool per sweep: with a
 * five second connect timeout, three dead pools dialled back to back would
 * stall the pools that are up for fifteen seconds at a time.
 */
static bool pool_connect(GlobalState *GLOBAL_STATE, pool_id_t id)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;
    secondary_state_t *st = &s_state[id];

    if (!st->fo_ready) {
        pool_failover_init(&st->fo, POOLB_MAX_RETRIES,
                           module->pool[id].fb_url != NULL &&
                           module->pool[id].fb_url[0] != 0);
        st->fo_ready = true;
    }

    int endpoint = pool_failover_endpoint(&st->fo);
    if (endpoint < 0) {
        /* both endpoints exhausted this cycle; this pool's share of the work
         * goes to the others meanwhile, which beats idling the ASIC */
        pool_failover_step(&st->fo, PF_EV_DISCONNECTED);
        st->next_attempt_us = esp_timer_get_time() + POOLB_RETRY_DELAY_MS * 1000LL;
        return false;
    }

    bool use_fb = (endpoint == 1);
    module->pool[id].is_using_failover = use_fb;

    char *url     = use_fb ? module->pool[id].fb_url  : module->pool[id].url;
    uint16_t port = use_fb ? module->pool[id].fb_port : module->pool[id].port;
    char *user    = use_fb ? module->pool[id].fb_user : module->pool[id].user;
    char *pass    = use_fb ? module->pool[id].fb_pass : module->pool[id].pass;
    if (url == NULL || url[0] == '\0') {
        st->next_attempt_us = esp_timer_get_time() + POOLB_IDLE_DELAY_MS * 1000LL;
        return false;
    }

    tls_mode tls = (tls_mode)(use_fb ? module->pool[id].fb_tls : module->pool[id].tls);

    /*
     * Build and connect on a local handle. Publishing a half-open handle to
     * the session would let the submit path write to a socket that never came
     * up.
     */
    esp_transport_handle_t transport = STRATUM_V1_transport_init(tls, module->pool[id].cert);
    if (transport == NULL) {
        ESP_LOGE(TAG, "%s transport init failed", pool_name(id));
        pool_failover_step(&st->fo, PF_EV_DISCONNECTED);
        st->next_attempt_us = esp_timer_get_time() + POOLB_RETRY_DELAY_MS * 1000LL;
        return false;
    }
    if (tls != DISABLED) {
        esp_transport_ssl_set_common_name(transport, url);
    }

    ESP_LOGI(TAG, "%s connecting to %s:%u", pool_name(id), url, port);
    if (esp_transport_connect(transport, url, port, POOLB_CONNECT_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "%s connect failed %s:%u", pool_name(id), url, port);
        esp_transport_close(transport);
        esp_transport_destroy(transport);
        pool_failover_step(&st->fo, PF_EV_DISCONNECTED);
        st->next_attempt_us = esp_timer_get_time() + POOLB_RETRY_DELAY_MS * 1000LL;
        return false;
    }

    pthread_mutex_lock(&GLOBAL_STATE->pool_session[id].transport_lock);
    /* Clear before publishing the handle: a failure recorded against the
     * session that just ended must not take down the one replacing it. The
     * same goes for a drop asked for while the old session was on its way
     * out -- it has already been honoured. */
    GLOBAL_STATE->pool_session[id].write_failed = false;
    GLOBAL_STATE->pool_session[id].drop_requested = false;
    GLOBAL_STATE->pool_session[id].transport = transport;
    pthread_mutex_unlock(&GLOBAL_STATE->pool_session[id].transport_lock);
    module->pool[id].connected = true;
    pool_failover_step(&st->fo, PF_EV_CONNECTED);
    ESP_LOGI(TAG, "%s connected to %s:%u (%s)", pool_name(id), url, port,
             use_fb ? "failover" : "primary");

    /* a fresh accumulator per connection */
    if (st->rxbuf != NULL) {
        free(st->rxbuf);
        st->rxbuf = NULL;
        st->rxsize = 0;
    }

    PoolSession *ses = &GLOBAL_STATE->pool_session[id];
    ses->send_uid = 1;
    STRATUM_V1_configure_version_rolling(ses->transport, ses->send_uid++, &ses->version_mask);
    STRATUM_V1_subscribe(ses->transport, ses->send_uid++, GLOBAL_STATE->asic_model_str);
    STRATUM_V1_authorize(ses->transport, ses->send_uid++, user, pass);

    /*
     * Ask for extranonce updates, if the owner turned them on.
     *
     * The handler for MINING_SET_EXTRANONCE below has always been here, so a
     * secondary pool could receive a rotation -- it simply never asked to. The
     * setting existed too: poolbxnsub is read out of NVS at startup into
     * pool[].extranonce_subscribe, and then nothing in the firmware ever
     * looked at that field. Pool A and the fallback both do this properly.
     *
     * Without it a pool keeps whatever extranonce it was given when it
     * subscribed. Against a pool that rotates them the work goes stale and the
     * shares are refused, which reads as a pool problem rather than as a
     * request we never sent.
     *
     * Sent straight after authorize rather than waiting for its reply, as pool
     * A does: stratum is ordered over the connection, so the pool still
     * processes the authorize first.
     */
    if (module->pool[id].extranonce_subscribe) {
        STRATUM_V1_extranonce_subscribe(ses->transport, ses->send_uid++);
    }
    return true;
}

/* Drop this pool's work, and only this pool's. */
static void pool_abandon_work(GlobalState *GLOBAL_STATE, pool_id_t id)
{
    if (GLOBAL_STATE->pool_session[id].queue.count > 0) {
        queue_clear(&GLOBAL_STATE->pool_session[id].queue);
    }
    for (uint32_t chain = 0; chain < MAX_CHAIN_NUM; chain++) {
        if (!GLOBAL_STATE->chain_pluged[chain]) {
            continue;
        }
        pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock[chain]);
        ASIC_jobs_queue_clear_pool(&GLOBAL_STATE->ASIC_jobs_queue[chain], id);
        for (int i = 0; i < 128; i++) {
            if (GLOBAL_STATE->valid_jobs[chain] != NULL &&
                GLOBAL_STATE->job_pool[chain][i] == id) {
                GLOBAL_STATE->valid_jobs[chain][i] = 0;
            }
        }
        pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain]);
    }
}

/*
 * Handle one message. Returns false if the session should be torn down.
 */
static bool pool_handle(GlobalState *GLOBAL_STATE, pool_id_t id,
                        StratumApiV1Message *msg)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;
    PoolSession *ses = &GLOBAL_STATE->pool_session[id];

    if (msg->method == MINING_NOTIFY) {
        if (msg->should_abandon_work) {
            /*
             * The mirror of cleanQueue(): this pool abandons its own work and
             * leaves every other pool's alone. Without it, stale jobs stayed
             * valid and their nonces were submitted to a pool that had already
             * moved on -- rejected as stale.
             */
            pool_abandon_work(GLOBAL_STATE, id);
        }
        /* Poll rather than test-then-block: create_jobs_task drains this
         * same queue, and if it emptied between the count test and the
         * dequeue this task -- the only producer -- waited forever on a
         * queue only it could fill, taking every secondary session with
         * it. A NULL here just means someone else already made room. */
        if (ses->queue.count == QUEUE_SIZE) {
            mining_notify *stale = (mining_notify *) queue_dequeue_timeout(&ses->queue, 0);
            if (stale != NULL) {
                STRATUM_V1_free_mining_notify(stale);
            }
        }
        msg->mining_notification->difficulty = ses->stratum_difficulty;
        queue_enqueue(&ses->queue, msg->mining_notification);
        /* the queue owns it now */
        msg->mining_notification = NULL;

    } else if (msg->method == MINING_SET_DIFFICULTY) {
        if (msg->new_difficulty != ses->stratum_difficulty) {
            ses->stratum_difficulty = msg->new_difficulty;
            ESP_LOGI(TAG, "%s difficulty: %lu", pool_name(id),
                     (unsigned long) ses->stratum_difficulty);
        }

    } else if (msg->method == MINING_SET_VERSION_MASK ||
               msg->method == STRATUM_RESULT_VERSION_MASK) {
        ses->version_mask = msg->version_mask;

    } else if (msg->method == STRATUM_RESULT_SUBSCRIBE ||
               msg->method == MINING_SET_EXTRANONCE) {
        if (msg->extranonce_str != NULL && msg->extranonce_2_len > 0) {
            /*
             * Swap under the lock create_jobs_task copies it under, so a job
             * never reads the string while it is being freed.
             */
            pthread_mutex_lock(&ses->extranonce_lock);
            char *old = ses->extranonce_str;
            ses->extranonce_str = strdup(msg->extranonce_str);
            ses->extranonce_2_len = msg->extranonce_2_len;
            pthread_mutex_unlock(&ses->extranonce_lock);
            free(old);
            ESP_LOGI(TAG, "%s extranonce %s, en2_len %d", pool_name(id),
                     ses->extranonce_str, ses->extranonce_2_len);
        }

    } else if (msg->method == STRATUM_RESULT) {
        /* share acknowledgements; setup replies arrive as _SETUP */
        if (msg->response_success) {
            module->pool[id].shares_accepted++;
        } else {
            module->pool[id].shares_rejected++;
            ESP_LOGW(TAG, "%s share rejected: %s", pool_name(id),
                     msg->error_str ? msg->error_str : "unknown");
        }

    } else if (msg->method == STRATUM_RESULT_SETUP) {
        if (!msg->response_success) {
            ESP_LOGE(TAG, "%s setup rejected: %s", pool_name(id),
                     msg->error_str ? msg->error_str : "unknown");
        }

    } else if (msg->method == CLIENT_RECONNECT) {
        ESP_LOGW(TAG, "%s requested reconnect", pool_name(id));
        return false;
    }
    return true;
}

/*
 * Give one connected pool a slice of attention. Returns false if its session
 * died and has been closed.
 */
static bool pool_service(GlobalState *GLOBAL_STATE, pool_id_t id,
                         StratumApiV1Message *msg)
{
    secondary_state_t *st = &s_state[id];
    char *line = NULL;

    /*
     * A share write failed since the last visit, so the peer is gone even
     * though the socket has not said so. Acting on it here rather than
     * waiting for the read side matters: a peer that disappears without a
     * FIN leaves the socket merely quiet, and quiet is what a healthy pool
     * between notifies looks like. lwIP will work it out when its
     * retransmits run out, minutes later, and until then this pool keeps a
     * full share of the slices and loses every one.
     */
    if (GLOBAL_STATE->pool_session[id].write_failed ||
        GLOBAL_STATE->pool_session[id].drop_requested) {
        /* Which one fired decides only what gets logged. Both end the session
         * by the same path on purpose: a drop asked for over the API is only
         * worth having if it exercises the teardown a real failure takes. */
        bool asked = GLOBAL_STATE->pool_session[id].drop_requested;

        GLOBAL_STATE->pool_session[id].write_failed = false;
        GLOBAL_STATE->pool_session[id].drop_requested = false;

        if (asked) {
            ESP_LOGW(TAG, "%s: dropping the session, asked over the API",
                     pool_name(id));
        } else {
            ESP_LOGW(TAG, "%s: a share could not be written, dropping the session",
                     pool_name(id));
        }
        pool_close(GLOBAL_STATE, id);
        pool_failover_step(&st->fo, PF_EV_DISCONNECTED);
        st->next_attempt_us = esp_timer_get_time() + POOLB_RETRY_DELAY_MS * 1000LL;
        return false;
    }

    stratum_rx_t rc = STRATUM_V1_poll_jsonrpc_line_ctx(
        GLOBAL_STATE->pool_session[id].transport,
        &st->rxbuf, &st->rxsize, POOL_RX_SLICE_MS, &line);

    if (rc == STRATUM_RX_ERROR) {
        ESP_LOGW(TAG, "%s connection lost, reconnecting", pool_name(id));
        pool_close(GLOBAL_STATE, id);
        pool_failover_step(&st->fo, PF_EV_DISCONNECTED);
        st->next_attempt_us = esp_timer_get_time() + POOLB_RETRY_DELAY_MS * 1000LL;
        return false;
    }
    if (rc == STRATUM_RX_IDLE) {
        return true;
    }

    STRATUM_V1_parse(msg, line);
    free(line);

    bool keep = pool_handle(GLOBAL_STATE, id, msg);

    /* The strings the parse allocated into msg. The struct is reused for
     * every line this task reads, so without this each rejection reason and
     * each extranonce is overwritten and lost. */
    STRATUM_V1_free_message(msg);

    if (!keep) {
        pool_close(GLOBAL_STATE, id);
        pool_failover_step(&st->fo, PF_EV_DISCONNECTED);
        st->next_attempt_us = esp_timer_get_time() + POOLB_RETRY_DELAY_MS * 1000LL;
        return false;
    }
    return true;
}

void stratum_poolb_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *) pvParameters;
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;
    StratumApiV1Message msg = {0};

    memset(s_state, 0, sizeof(s_state));

    ESP_LOGI(TAG, "secondary pool task started");

    while (1) {
        if (!GLOBAL_STATE->dual_enable || !GLOBAL_STATE->ASIC_initalized ||
            !network_is_connected()) {
            for (pool_id_t id = POOL_B; id < POOL_MAX; id++) {
                if (module->pool[id].connected) {
                    ESP_LOGI(TAG, "%s dropped: secondary mining is off", pool_name(id));
                    pool_close(GLOBAL_STATE, id);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(POOLB_IDLE_DELAY_MS));
            continue;
        }

        int connected = 0;
        bool dialled = false;

        for (pool_id_t id = POOL_B; id < POOL_MAX; id++) {
            bool wanted = pool_configured(module, id) && pool_allowed(GLOBAL_STATE, id);

            if (!wanted) {
                if (module->pool[id].connected) {
                    ESP_LOGI(TAG, "%s dropped: no longer configured", pool_name(id));
                    pool_close(GLOBAL_STATE, id);
                }
                continue;
            }

            if (module->pool[id].connected) {
                connected++;
                continue;
            }

            /* One dial per sweep, so a dead pool cannot stall a live one. */
            if (!dialled && esp_timer_get_time() >= s_state[id].next_attempt_us) {
                dialled = true;
                if (pool_connect(GLOBAL_STATE, id)) {
                    connected++;
                }
            }
        }

        if (connected == 0) {
            vTaskDelay(pdMS_TO_TICKS(POOLB_IDLE_DELAY_MS));
            continue;
        }

        for (pool_id_t id = POOL_B; id < POOL_MAX; id++) {
            if (!module->pool[id].connected) {
                continue;
            }
            pool_service(GLOBAL_STATE, id, &msg);
        }
    }

    vTaskDelete(NULL);
}
