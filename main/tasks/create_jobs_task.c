#include <sys/time.h>
#include <limits.h>

#include "work_queue.h"
#include "pool_scheduler.h"

/* global_state.h cannot include pool_scheduler.h -- components/asic and
 * components/bc_hal include it and do not see dual_pool -- so SystemModule
 * declares its pool array with a literal. This is what stops the two from
 * drifting apart. */
_Static_assert(POOL_MAX == 4, "SystemModule::pool[] must match POOL_MAX");
#include "global_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mining.h"
#include "string.h"
#include "stratum_task.h"     /* stratum_connected_status() */

#include "asic.h"

static const char *TAG = "create_jobs_task";

/*
 * How long to wait on pool A before going round anyway.
 *
 * Long enough that a busy pool A costs no extra wakeups, short enough that
 * the secondary pools keep being served while pool A is away. Jobs are
 * buffered to QUEUE_LOW_WATER_MARK, so a gap this size is invisible.
 */
#define POOL_A_WAIT_MS 250


#define QUEUE_LOW_WATER_MARK 10 // Adjust based on your requirements

static bool should_generate_more_work(GlobalState *GLOBAL_STATE, uint32_t chain_num);
static void generate_work(GlobalState *GLOBAL_STATE, mining_notify *notification, uint64_t extranonce_2, uint32_t difficulty, uint32_t chain_num, uint8_t pool_id);

/* A pool's weight, clamped to what the scheduler's uint8_t array holds. */
static uint8_t pool_weight(const SystemModule *module, pool_id_t id)
{
    uint16_t w = module->pool[id].weight;
    return (uint8_t) (w > 100 ? 100 : w);
}

/*
 * Work out each pool's share of the slices.
 *
 * dual_ratio_a stays what it always was -- pool A's percentage -- because that
 * is what is stored in NVS and what the UI shows. Whatever is left over is
 * divided among the pools that are configured, by their own weights if any
 * have been set and evenly if none have. The scheduler normalises, so these
 * figures only have to be in the right proportion to each other.
 *
 * Weights come from what is *configured*, not from what is currently
 * connected. A pool that is down keeps being chosen and keeps failing the
 * liveness test at the call site, which is what makes jobs_selected outrun
 * jobs_served and puts a visible number on the shortfall. Zeroing a dead
 * pool's weight here would quietly paper over exactly that.
 */
static void build_pool_weights(GlobalState *GLOBAL_STATE, uint8_t *weights)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;
    int configured = 0;
    int stated = 0;

    memset(weights, 0, POOL_MAX);

    /*
     * Clamped on the way in, because `weights` is a uint8_t array and the
     * stored weight is a uint16_t. Above 255 it wraps -- 256 becomes 0, so a
     * pool the owner asked for never wins a slice, while `stated` below still
     * sees the untruncated value and so does not fall back to an even split
     * either. The settings handler clamps too; this is the one that holds for
     * a value written straight to NVS.
     */
    for (pool_id_t id = POOL_B; id < POOL_MAX; id++) {
        const char *url = module->pool[id].url;
        if (url != NULL && url[0] != '\0') {
            configured++;
            stated += pool_weight(module, id);
        }
    }

    if (configured == 0) {
        weights[POOL_A] = 100;
        return;
    }

    uint8_t ratio_a = GLOBAL_STATE->dual_ratio_a;
    if (ratio_a > 100) {
        ratio_a = 100;
    }
    weights[POOL_A] = ratio_a;

    uint8_t rest = (uint8_t) (100 - ratio_a);
    for (pool_id_t id = POOL_B; id < POOL_MAX; id++) {
        const char *url = module->pool[id].url;
        if (url == NULL || url[0] == '\0') {
            continue;
        }
        weights[id] = (stated > 0) ? pool_weight(module, id)
                                   : (uint8_t) (rest / configured);
    }
}

void create_jobs_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;
    uint32_t difficulty = GLOBAL_STATE->stratum_difficulty;

    ESP_LOGI(TAG, "create_jobs_task started");

    uint64_t extranonce_2 = 0;

    /* ---- the pools that are not pool A ----
     * Each one's latest notify is held here rather than dequeued in step with
     * pool A's: the pools issue work on their own schedules, and pool A's
     * dequeue below blocks. Indexed by pool id; [POOL_A] is unused, as
     * everywhere else. */
    mining_notify *secondary_notify[POOL_MAX] = {0};
    uint64_t secondary_extranonce_2[POOL_MAX] = {0};
    uint32_t secondary_difficulty[POOL_MAX] = {0};

    pool_scheduler_t scheduler;
    uint8_t sched_weights[POOL_MAX] = {0};
    uint16_t sched_interval = 0;
    bool sched_ready = false;

    /*
     * Pool A's latest notify, held the same way the others are.
     *
     * This used to be a blocking dequeue, and that made pool A a single point
     * of failure for every pool: this is the only task that builds work for
     * any of them, so while pool A was silent -- a disconnect, a slow
     * reconnect, a DNS failure -- it sat here and pools B, C and D held open
     * sockets with nothing to hash. The whole point of mining several pools
     * is that one of them going away is survivable.
     *
     * Now it waits a bounded time, keeps whatever pool A last sent, and goes
     * round regardless. With no pool A work yet there is still nothing to
     * build for pool A, but the secondaries are served from their own held
     * notifies as usual.
     */
    mining_notify *mining_notification = NULL;

    while (1)
    {
        mining_notify *fresh = (mining_notify *)
            queue_dequeue_timeout(&GLOBAL_STATE->stratum_queue, POOL_A_WAIT_MS);

        if (fresh != NULL) {
            if (mining_notification != NULL) {
                STRATUM_V1_free_mining_notify(mining_notification);
            }
            mining_notification = fresh;
            ESP_LOGD(TAG, "New Work Dequeued %s", mining_notification->job_id);
        }

        /*
         * Pool A's session has ended, so what it last sent is not work this
         * pool will accept any more: the job id and the extranonce belonged to
         * that session. Let it go, so the NULL test further down starts
         * skipping pool A's slices.
         *
         * Without this the held notify outlived the session. That NULL test
         * then only ever fired before pool A's *first* notify since boot, so a
         * pool A that worked and then dropped kept being given its slices and
         * kept building from a dead job -- counted as served, submitted, and
         * refused as an unknown job id. At the default 50% split that is half
         * the miner's output going nowhere, with the counters saying otherwise.
         * The secondaries got this fix in 5325e52; pool A did not.
         *
         * The earlier outage test missed it by pointing pool A at an
         * unroutable address from boot, which is the one arrangement where the
         * pointer is never set and the old test did fire.
         *
         * stratum_connected_status() rather than the transport: pool A's
         * transport has no lock, and reading a bool that another task writes is
         * the pattern already used for write_failed. It is cleared when that
         * task's connection loop ends and set when a line arrives, and a notify
         * is only ever queued after it is set -- so a fresh dequeue above can
         * never be discarded by this. stratum_close_connection() clears the
         * queue as well, which is why the held pointer is the only stale state
         * left to deal with here.
         */
        if (mining_notification != NULL && !stratum_connected_status()) {
            STRATUM_V1_free_mining_notify(mining_notification);
            mining_notification = NULL;
            ESP_LOGI(TAG, "pool A disconnected; dropped its held work, "
                          "skipping its slices until it sends again");
        }

        /*
         * No early exit when pool A has nothing.
         *
         * This used to `continue` here, which was the same bug in a new
         * place: pool A never sends while it is down, so the loop turned
         * round forever and never reached the job building below -- the one
         * thing that serves pools B, C and D. Tested by pointing pool A's
         * primary and failover at an unroutable address: all three
         * secondaries connected and subscribed, and the miner sat at 0 GH/s
         * for four minutes with not one share from any of them.
         *
         * Pool A's own slices are skipped further down while it has no
         * notify to build from. Everything else proceeds.
         */
        if (GLOBAL_STATE->new_set_mining_difficulty_msg)
        {
            ESP_LOGI(TAG, "New job difficulty %lu", GLOBAL_STATE->stratum_difficulty);
            difficulty = GLOBAL_STATE->stratum_difficulty;
            GLOBAL_STATE->new_set_mining_difficulty_msg = false;
        }

        if (GLOBAL_STATE->new_stratum_version_rolling_msg) {
            ESP_LOGI(TAG, "Set chip version rolls %i", (int)(GLOBAL_STATE->version_mask >> 13));
            ASIC_set_version_mask(GLOBAL_STATE, GLOBAL_STATE->version_mask);
            GLOBAL_STATE->new_stratum_version_rolling_msg = false;
        }

        /*
         * Time-boxed, so the outer poll above runs even when pool A is silent.
         *
         * The exit condition is "pool A has queued something", which never
         * becomes true while pool A is down -- so without a deadline this
         * loop would keep the secondaries fed but never look to see whether
         * pool A had come back. A healthy pool A leaves long before this
         * expires, so it costs nothing in the normal case.
         */
        int64_t inner_deadline_us = esp_timer_get_time() + 1000 * 1000;

        while (GLOBAL_STATE->stratum_queue.count < 1 &&
               GLOBAL_STATE->abandon_work == 0 &&
               esp_timer_get_time() < inner_deadline_us)
        {
            for(uint32_t chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++)
            {
                if(GLOBAL_STATE->chain_pluged[chain_num])
                {
                    if (should_generate_more_work(GLOBAL_STATE, chain_num))
                    {
                        uint8_t pool_id = POOL_A;

                        if (GLOBAL_STATE->dual_enable) {
                            /* re-init when the weights or slice length change,
                             * so tuning them does not need a restart */
                            uint8_t weights[POOL_MAX];
                            build_pool_weights(GLOBAL_STATE, weights);
                            if (!sched_ready ||
                                sched_interval != GLOBAL_STATE->dual_interval_ms ||
                                memcmp(sched_weights, weights, sizeof(weights)) != 0) {
                                memcpy(sched_weights, weights, sizeof(weights));
                                sched_interval = GLOBAL_STATE->dual_interval_ms;
                                pool_scheduler_init_n(&scheduler, sched_weights, POOL_MAX,
                                                      sched_interval, esp_timer_get_time());
                                sched_ready = true;
                            }

                            /* take each pool's newest notify if one has arrived */
                            for (pool_id_t id = POOL_B; id < POOL_MAX; id++) {
                                /*
                                 * A notify belongs to the session that sent
                                 * it. When that session goes, so does the
                                 * notify -- otherwise this task keeps the
                                 * dead one indefinitely and treats the pool
                                 * as having work, so the moment the new
                                 * transport is published it builds from the
                                 * previous session's job id and the pool
                                 * refuses every share built that way.
                                 */
                                bool session_up = false;
                                pthread_mutex_lock(&GLOBAL_STATE->pool_session[id].transport_lock);
                                session_up = (GLOBAL_STATE->pool_session[id].transport != NULL);
                                pthread_mutex_unlock(&GLOBAL_STATE->pool_session[id].transport_lock);

                                if (!session_up && secondary_notify[id] != NULL) {
                                    STRATUM_V1_free_mining_notify(secondary_notify[id]);
                                    secondary_notify[id] = NULL;
                                    secondary_difficulty[id] = 0;
                                    secondary_extranonce_2[id] = 0;
                                }

                                /* Polled, not a count test followed by a
                                 * blocking dequeue: that pool's own task
                                 * clears this queue, and a clear landing
                                 * between the two left this task -- the only
                                 * one that builds work for ANY pool -- parked
                                 * on an empty queue. */
                                for (;;) {
                                    mining_notify *fresh = (mining_notify *)
                                        queue_dequeue_timeout(&GLOBAL_STATE->pool_session[id].queue, 0);
                                    if (fresh == NULL) break;
                                    if (secondary_notify[id] != NULL) {
                                        STRATUM_V1_free_mining_notify(secondary_notify[id]);
                                    }
                                    secondary_notify[id] = fresh;
                                    secondary_difficulty[id] = GLOBAL_STATE->pool_session[id].stratum_difficulty;
                                    secondary_extranonce_2[id] = 0;
                                }
                            }

                            /*
                             * The chosen pool has to still be there, not merely
                             * to have sent something once. secondary_notify[]
                             * keeps the last notify for as long as the miner
                             * runs, so on its own it stays true across a
                             * disconnect and the scheduler carries on handing
                             * that pool its share of the slices -- building work
                             * against a job that cannot be submitted, on an
                             * extranonce that will not survive the reconnect.
                             * Any nonce found against it reaches the submit
                             * path, finds the transport NULL and is dropped.
                             *
                             * Nothing reports that. jobs_served[] would keep
                             * counting, so the miner would look like it was
                             * splitting work as configured while that pool's
                             * share of the hashrate produced nothing at all,
                             * until it happened to come back.
                             *
                             * The transport is published only once the
                             * connection is up and cleared when it goes down, so
                             * it is the honest test. Read it under its own lock:
                             * the pool task can swap it at any moment.
                             */
                            uint8_t chosen = pool_scheduler_select(&scheduler, esp_timer_get_time());
                            GLOBAL_STATE->jobs_selected[chosen]++;

                            if (chosen != POOL_A && secondary_notify[chosen] != NULL) {
                                bool up = false;
                                pthread_mutex_lock(&GLOBAL_STATE->pool_session[chosen].transport_lock);
                                up = (GLOBAL_STATE->pool_session[chosen].transport != NULL);
                                pthread_mutex_unlock(&GLOBAL_STATE->pool_session[chosen].transport_lock);
                                if (up) {
                                    pool_id = chosen;
                                }
                            }
                            /* chosen but not served means the job fell through
                             * to pool A; the counters make that visible */
                        }
                        if (pool_id == POOL_A && mining_notification == NULL) {
                            /*
                             * Pool A's slice with nothing of pool A's to
                             * build from -- it is down, or has not sent its
                             * first notify yet. Skip the slice rather than
                             * dereference the notify, and do not count it as
                             * served: jobs_selected outrunning jobs_served is
                             * exactly how a pool's shortfall is meant to
                             * show. A short yield so a dead pool A cannot
                             * spin this loop.
                             */
                            vTaskDelay(10 / portTICK_PERIOD_MS);
                            continue;
                        }

                        GLOBAL_STATE->jobs_served[pool_id]++;

                        if (pool_id != POOL_A) {
                            generate_work(GLOBAL_STATE, secondary_notify[pool_id],
                                          secondary_extranonce_2[pool_id],
                                          secondary_difficulty[pool_id] ? secondary_difficulty[pool_id] : difficulty,
                                          chain_num, pool_id);
                            secondary_extranonce_2[pool_id]++;
                        } else {
                            generate_work(GLOBAL_STATE, mining_notification, extranonce_2, difficulty, chain_num, POOL_A);
                            extranonce_2++;
                        }
                    }
                    else
                    {
                        // If no more work needed, wait a bit before checking again.
                        vTaskDelay(100 / portTICK_PERIOD_MS);
                    }
                }
            }
        }

        if (GLOBAL_STATE->abandon_work == 1)
        {
            GLOBAL_STATE->abandon_work = 0;
            for(uint32_t chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++)
            {
                if(GLOBAL_STATE->chain_pluged[chain_num])
                {    
                    ESP_LOGD(TAG, "abandon work. %"PRIu32"", chain_num);
                    /* abandon_work is raised by pool A's clean_jobs, so it
                     * discards pool A's work only -- see cleanQueue() */
                    if (GLOBAL_STATE->dual_enable) {
                        ASIC_jobs_queue_clear_pool(&GLOBAL_STATE->ASIC_jobs_queue[chain_num], POOL_A);
                    } else {
                        ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue[chain_num]);
                    }
                    xSemaphoreGive(GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].semaphore);
                }
            }
        }

        /* Deliberately not freed here: it is pool A's latest notify and is
         * held until pool A sends another, exactly as each secondary pool's
         * is. Freeing it per pass is what forced the blocking dequeue. */
    }
}

static bool should_generate_more_work(GlobalState *GLOBAL_STATE, uint32_t chain_num)
{
    return GLOBAL_STATE->ASIC_jobs_queue[chain_num].count < QUEUE_LOW_WATER_MARK;
}

static void generate_work(GlobalState *GLOBAL_STATE, mining_notify *notification, uint64_t extranonce_2, uint32_t difficulty, uint32_t chain_num, uint8_t pool_id)
{
    int extranonce_2_len = 0;
    char extranonce_str[40] = {"\0"};
    /* Each pool has its own extranonce and version mask. Building a job for one
     * pool with another's values yields a share it cannot match to a job. */
    uint32_t version_mask = (pool_id != POOL_A)
                            ? GLOBAL_STATE->pool_session[pool_id].version_mask : GLOBAL_STATE->version_mask;

    if (pool_id != POOL_A) {
        pthread_mutex_lock(&GLOBAL_STATE->pool_session[pool_id].extranonce_lock);
        extranonce_2_len = GLOBAL_STATE->pool_session[pool_id].extranonce_2_len;
        if (GLOBAL_STATE->pool_session[pool_id].extranonce_str != NULL) {
            snprintf(extranonce_str, sizeof(extranonce_str), "%s", GLOBAL_STATE->pool_session[pool_id].extranonce_str);
        }
        pthread_mutex_unlock(&GLOBAL_STATE->pool_session[pool_id].extranonce_lock);
        if (extranonce_str[0] == '\0' || extranonce_2_len <= 0) {
            /* that pool has not subscribed yet; nothing to build from */
            return;
        }
    } else if(xSemaphoreTake(GLOBAL_STATE->global_parameter_mutex, pdMS_TO_TICKS(2000))){
        extranonce_2_len = GLOBAL_STATE->extranonce_2_len;
        snprintf(extranonce_str, sizeof(extranonce_str), "%s", GLOBAL_STATE->extranonce_str);
        xSemaphoreGive(GLOBAL_STATE->global_parameter_mutex);
    }else{
        ESP_LOGE(TAG, "Failed to get the global_parameter_mutex.");
        goto out;
    }

    char *extranonce_2_str = extranonce_2_generate(extranonce_2, extranonce_2_len);
    if (extranonce_2_str == NULL) {
        ESP_LOGE(TAG, "Failed to generate extranonce_2");
        return;
    }

    char *coinbase_tx = construct_coinbase_tx(notification->coinbase_1, notification->coinbase_2, extranonce_str, extranonce_2_str);
    if (coinbase_tx == NULL) {
        ESP_LOGE(TAG, "Failed to construct coinbase_tx");
        free(extranonce_2_str);
        return;
    }

    char *merkle_root = calculate_merkle_root_hash(coinbase_tx, (uint8_t(*)[32])notification->merkle_branches, notification->n_merkle_branches);
    if (merkle_root == NULL) {
        ESP_LOGE(TAG, "Failed to calculate merkle_root");
        free(extranonce_2_str);
        free(coinbase_tx);
        return;
    }

#if 1
    bm_job next_job = construct_bm_job(notification, merkle_root, version_mask, difficulty);
#else
    bm_job next_job = construct_ltc_job(notification, merkle_root);
#endif

    bm_job *queued_next_job = malloc(sizeof(bm_job));
    if (queued_next_job == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for queued_next_job");
        free(extranonce_2_str);
        free(coinbase_tx);
        free(merkle_root);
        return;
    }

    memcpy(queued_next_job, &next_job, sizeof(bm_job));
    queued_next_job->extranonce2 = extranonce_2_str; // Transfer ownership
    queued_next_job->jobid = strdup(notification->job_id);
    queued_next_job->version_mask = version_mask;
    queued_next_job->pool_id = pool_id;
    queue_enqueue(&GLOBAL_STATE->ASIC_jobs_queue[chain_num], queued_next_job);

    free(coinbase_tx);
    free(merkle_root);

out:
    return;
}