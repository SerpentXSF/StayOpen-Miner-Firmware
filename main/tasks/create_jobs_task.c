#include <sys/time.h>
#include <limits.h>

#include "work_queue.h"
#include "pool_scheduler.h"
#include "global_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mining.h"
#include "string.h"

#include "asic.h"

static const char *TAG = "create_jobs_task";

#define QUEUE_LOW_WATER_MARK 10 // Adjust based on your requirements

static bool should_generate_more_work(GlobalState *GLOBAL_STATE, uint32_t chain_num);
static void generate_work(GlobalState *GLOBAL_STATE, mining_notify *notification, uint64_t extranonce_2, uint32_t difficulty, uint32_t chain_num, uint8_t pool_id);

void create_jobs_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;
    uint32_t difficulty = GLOBAL_STATE->stratum_difficulty;

    ESP_LOGI(TAG, "create_jobs_task started");

    uint64_t extranonce_2 = 0;

    /* ---- dual mining ----
     * Pool B's latest notify is held here rather than dequeued in step with
     * pool A's: the two pools issue work on their own schedules, and pool A's
     * dequeue below blocks. */
    mining_notify *poolb_notification = NULL;
    uint64_t extranonce_2_B = 0;
    uint32_t difficulty_B = 0;
    pool_scheduler_t scheduler;
    uint8_t sched_ratio = 0;
    uint16_t sched_interval = 0;
    bool sched_ready = false;

    while (1)
    {
        mining_notify *mining_notification = (mining_notify *)queue_dequeue(&GLOBAL_STATE->stratum_queue);
        if (mining_notification == NULL) {
            ESP_LOGE(TAG, "Failed to dequeue mining notification");
            vTaskDelay(100 / portTICK_PERIOD_MS); // Wait a bit before trying again
            continue;
        }

        ESP_LOGD(TAG, "New Work Dequeued %s", mining_notification->job_id);

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

        while (GLOBAL_STATE->stratum_queue.count < 1 && GLOBAL_STATE->abandon_work == 0)
        {
            for(uint32_t chain_num = 0; chain_num < MAX_CHAIN_NUM; chain_num++)
            {
                if(GLOBAL_STATE->chain_pluged[chain_num])
                {
                    if (should_generate_more_work(GLOBAL_STATE, chain_num))
                    {
                        uint8_t pool_id = POOL_A;

                        if (GLOBAL_STATE->dual_enable) {
                            /* re-init when the ratio or slice length changes, so
                             * tuning them does not need a restart */
                            if (!sched_ready ||
                                sched_ratio != GLOBAL_STATE->dual_ratio_a ||
                                sched_interval != GLOBAL_STATE->dual_interval_ms) {
                                sched_ratio = GLOBAL_STATE->dual_ratio_a;
                                sched_interval = GLOBAL_STATE->dual_interval_ms;
                                pool_scheduler_init(&scheduler, sched_ratio, sched_interval,
                                                    esp_timer_get_time());
                                sched_ready = true;
                            }

                            /* take pool B's newest notify if one has arrived */
                            while (GLOBAL_STATE->stratum_queueB.count > 0) {
                                mining_notify *fresh =
                                    (mining_notify *)queue_dequeue(&GLOBAL_STATE->stratum_queueB);
                                if (fresh == NULL) break;
                                if (poolb_notification != NULL) {
                                    STRATUM_V1_free_mining_notify(poolb_notification);
                                }
                                poolb_notification = fresh;
                                difficulty_B = GLOBAL_STATE->stratum_difficultyB;
                                extranonce_2_B = 0;
                            }

                            /*
                             * Pool B has to still be there, not merely to have
                             * sent something once. poolb_notification keeps the
                             * last notify for as long as the miner runs, so on
                             * its own it stays true across a disconnect and the
                             * scheduler carries on handing pool B its share of
                             * the slices -- building work against a job that
                             * cannot be submitted, on an extranonce that will
                             * not survive the reconnect. Any nonce found
                             * against it reaches the submit path, finds
                             * transportB NULL and is dropped.
                             *
                             * Nothing reports that. jobs_served[POOL_B] keeps
                             * counting, so the miner looks like it is splitting
                             * work as configured while dual_ratio_b of the
                             * hashrate produces nothing at all, until pool B
                             * happens to come back.
                             *
                             * transportB is published only once the connection
                             * is up and cleared when it goes down, so it is the
                             * honest test. Read it under its own lock: the pool
                             * B task can swap it at any moment.
                             */
                            bool poolb_up = false;
                            pthread_mutex_lock(&GLOBAL_STATE->transportB_lock);
                            poolb_up = (GLOBAL_STATE->transportB != NULL);
                            pthread_mutex_unlock(&GLOBAL_STATE->transportB_lock);

                            uint8_t chosen = pool_scheduler_select(&scheduler, esp_timer_get_time());
                            GLOBAL_STATE->jobs_selected[chosen]++;
                            if (chosen == POOL_B && poolb_notification != NULL &&
                                poolb_up) {
                                pool_id = POOL_B;
                            }
                            /* chosen but not served means the job went to the
                             * other pool; the counters make that visible */
                        }
                        GLOBAL_STATE->jobs_served[pool_id]++;

                        if (pool_id == POOL_B) {
                            generate_work(GLOBAL_STATE, poolb_notification, extranonce_2_B,
                                          difficulty_B ? difficulty_B : difficulty, chain_num, POOL_B);
                            extranonce_2_B++;
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

        STRATUM_V1_free_mining_notify(mining_notification);
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
     * pool with the other's values yields a share it cannot match to a job. */
    uint32_t version_mask = (pool_id == POOL_B)
                            ? GLOBAL_STATE->version_maskB : GLOBAL_STATE->version_mask;

    if (pool_id == POOL_B) {
        pthread_mutex_lock(&GLOBAL_STATE->extranonceB_lock);
        extranonce_2_len = GLOBAL_STATE->extranonce_2_lenB;
        if (GLOBAL_STATE->extranonce_strB != NULL) {
            snprintf(extranonce_str, sizeof(extranonce_str), "%s", GLOBAL_STATE->extranonce_strB);
        }
        pthread_mutex_unlock(&GLOBAL_STATE->extranonceB_lock);
        if (extranonce_str[0] == '\0' || extranonce_2_len <= 0) {
            /* pool B has not subscribed yet; nothing to build from */
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