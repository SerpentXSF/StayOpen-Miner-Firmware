#include <lwip/tcpip.h>
/* for shutdown() on pool A's socket -- see the share-submit path below */
#include "lwip/sockets.h"
#include "esp_transport.h"

#include "system.h"
#include "work_queue.h"
#include "pool_scheduler.h"
#include "serial.h"
#include "lt0051.h"
#include <string.h>
#include <pthread.h>
#include "esp_log.h"
#include "nvs_config.h"
#include "utils.h"
#include "stratum_task.h"
#include "hashrate_monitor_task.h"
#include "asic.h"

static const char *TAG = "asic_result";

void ASIC_result_task(void *pvParameters)
{ 
    uint32_t chain_num = ((AsicParam *)(pvParameters))->chain_num;
    GlobalState *GLOBAL_STATE = ((AsicParam *)(pvParameters))->p_global_state;
    ESP_LOGI(TAG, "ASIC_result_task initialising.");

    while (1)
    {
        //task_result *asic_result = (*GLOBAL_STATE->ASIC_functions.receive_result_fn)(GLOBAL_STATE);
        task_result *asic_result = ASIC_process_work(GLOBAL_STATE, chain_num);

        if (asic_result == NULL)
        {
            continue;
        }

        if (asic_result->register_type != REGISTER_INVALID) {
            hashrate_monitor_register_read(GLOBAL_STATE, asic_result->register_type, asic_result->asic_nr, asic_result->value);
            continue;
        }

        uint8_t job_id = asic_result->job_id;

        /*
         * Take everything this share needs while holding valid_jobs_lock.
         *
         * active_jobs[] is rewritten by whichever task sends work, and that
         * task frees the job already in the slot before storing the new one,
         * outside this lock -- the caveat already recorded on job_pool in
         * global_state.h, which is why pool ownership is kept in its own array
         * rather than read back through these pointers. Scoring and submitting
         * a share read them anyway.
         *
         * This is not a narrow window. job_id comes back from the ASIC, and
         * BM1370_send_work walks the slots in steps of 24 modulo 128, so only
         * sixteen are ever used and each is recycled every sixteen jobs. A
         * nonce arriving for a slot that is being reused is an ordinary event,
         * and reading the pointer out here unlocked meant scoring and
         * submitting a share from freed memory.
         *
         * jobid and extranonce2 are the only fields that point outside the
         * job, so they are copied and everything else is taken by value. That
         * keeps the lock off the socket writes below, which must not be held
         * across a pool that has stopped reading.
         */
        pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

        bm_job * job = GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs[job_id];
        if (GLOBAL_STATE->valid_jobs[chain_num][job_id] == 0 || job == NULL)
        {
            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);
            ESP_LOGW(TAG, "Invalid job nonce found, 0x%02X", job_id);
            continue;
        }

        double nonce_diff = test_nonce_value(job, asic_result->nonce,
                                             asic_result->rolled_version);

        uint32_t job_version   = job->version;
        uint32_t job_ntime     = job->ntime;
        uint32_t job_pool_diff = job->pool_diff;
        uint8_t  job_pool_id   = job->pool_id;
        char *   job_jobid     = job->jobid       ? strdup(job->jobid)       : NULL;
        char *   job_en2       = job->extranonce2 ? strdup(job->extranonce2) : NULL;

        pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

        if (job_jobid == NULL || job_en2 == NULL) {
            ESP_LOGW(TAG, "Out of memory copying job 0x%02X; share dropped", job_id);
            free(job_jobid);
            free(job_en2);
            continue;
        }

        //log the ASIC response
        /* Job id first: with two pools running it is what ties a nonce back to
         * the pool that issued the work it was found against. */
        ESP_LOGD(TAG, "ID: %s, ASIC nr: %d, Core: %d/%d, ver: %08" PRIX32 " Nonce %08" PRIX32 " diff %.1f of %ld.",
            job_jobid,
            asic_result->asic_nr, asic_result->core_id, asic_result->small_core_id,
            asic_result->rolled_version, asic_result->nonce, nonce_diff,
            job_pool_diff);

        if (nonce_diff >= job_pool_diff)
        {
            /*
             * A share belongs to the pool that issued its job. Sending one
             * pool's work to another gets it rejected as an unknown job id,
             * and would credit the wrong pool. The job carries pool_id for
             * exactly this reason.
             *
             * The test is "not pool A", not "is pool B". It was the latter
             * once, when pool B was the only other pool, and a third pool
             * made that silently wrong: a share from pool C or D is not
             * POOL_B, so it fell into the pool A branch below and was
             * submitted to pool A carrying another pool's job id, extranonce
             * and ntime. Measured on a BC04 mining four pools: C and D
             * reported zero shares over fourteen minutes where each should
             * have found about thirty, while pool A's reject rate went from
             * four in twenty thousand to thirty-one in two hundred and
             * forty-eight. Both pools looked broken; neither was.
             */
            if (GLOBAL_STATE->dual_enable && job_pool_id != POOL_A &&
                job_pool_id < POOL_MAX) {
                PoolSession *ses = &GLOBAL_STATE->pool_session[job_pool_id];

                /* Hold the pool's transport_lock across the write: its task
                 * can be tearing the handle down at any moment. */
                /*
                 * The worker this pool actually authorized as. A pool on its
                 * failover endpoint subscribed and authorized with fb_user --
                 * a different pool, so usually a different wallet and worker
                 * -- and submitting under the primary's name gets every share
                 * refused as an unauthorized worker for as long as the
                 * failover lasts. Pool A's branch below has always read its
                 * is_using_fallback for this reason; this one did not.
                 */
                const PoolConfig *cfg = &GLOBAL_STATE->SYSTEM_MODULE.pool[job_pool_id];
                char *pool_user = cfg->is_using_failover ? cfg->fb_user : cfg->user;

                pthread_mutex_lock(&ses->transport_lock);
                if (ses->transport != NULL) {
                    int ret2 = STRATUM_V1_submit_share(
                        ses->transport,
                        ses->send_uid++,
                        pool_user,
                        job_jobid,
                        job_en2,
                        job_ntime,
                        asic_result->nonce,
                        asic_result->rolled_version ^ job_version);
                    if (ret2 < 0) {
                        /* Tell the pool task: a write that fails means the
                         * peer is gone, and it will not find that out from
                         * the receive side for minutes. */
                        ses->write_failed = true;
                        ESP_LOGW(TAG, "Unable to write share to pool %c. Ret: %d",
                                 (char) ('A' + job_pool_id), ret2);
                    }
                } else {
                    ESP_LOGD(TAG, "pool %c share dropped: not connected",
                             (char) ('A' + job_pool_id));
                }
                pthread_mutex_unlock(&ses->transport_lock);
            } else {
                char * user = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user : GLOBAL_STATE->SYSTEM_MODULE.pool_user;

                /*
                 * stratum_close_connection() nulls this on any disconnect,
                 * and work built before that is still in flight afterwards.
                 * The secondary branch above has always checked; this one
                 * wrote regardless, which was survivable only because the
                 * blocking dequeue meant no work was built while pool A was
                 * down. It is built now, so the check has to be here.
                 */
                /* Under pool A's lock, for the reason the secondary branch
                 * above gives: stratum_task can be tearing this handle down
                 * at any moment. */
                pthread_mutex_lock(&GLOBAL_STATE->transport_lock);
                if (GLOBAL_STATE->transport == NULL) {
                    ESP_LOGD(TAG, "pool A share dropped: not connected");
                } else {
                    /*TODO: submit a share without rolled version.*/
                    int ret = STRATUM_V1_submit_share(
                        GLOBAL_STATE->transport,
                        GLOBAL_STATE->send_uid++,
                        user,
                        job_jobid,
                        job_en2,
                        job_ntime,
                        asic_result->nonce,
                        asic_result->rolled_version ^ job_version);

                    if (ret < 0) {
                        /*
                         * This used to call stratum_close_connection(), which
                         * destroyed the handle from this task while
                         * stratum_task could be blocked in
                         * esp_transport_read() on it. Ask instead.
                         */
                        ESP_LOGW(TAG, "Unable to write share to pool A. Ret: %d", ret);
                        GLOBAL_STATE->write_failed = true;

                        /*
                         * And wake the reader, because the flag alone can go
                         * unread for ten minutes: stratum_task's receive loops
                         * on a 60 s poll up to STRATUM_IDLE_LIMIT_MS, and for
                         * all of it pool A keeps its full share of the slices
                         * while every share built for it is dropped. (Not
                         * SO_RCVTIMEO, which is also 600 s but barely
                         * participates -- recv() is only reached once select()
                         * says readable.)
                         *
                         * SHUT_RD, not SHUT_RDWR: the intent is to wake the
                         * reader and let its owner tear the session down.
                         * SHUT_RDWR is treated as a close -- it drops the pcb
                         * and aborts a write in flight with ERR_CLSD -- which
                         * is more than is wanted from here.
                         *
                         * Two things make this safe from a task that must not
                         * destroy. The fd cannot have been recycled: a
                         * non-NULL handle observed under this lock implies the
                         * fd is still this session's, because the only code
                         * that closes it nulls the handle in the same critical
                         * section. And one socket may be used from two tasks
                         * at all only because the IDF port sets
                         * LWIP_NETCONN_FULLDUPLEX=1 with
                         * LWIP_NETCONN_SEM_PER_THREAD=1 (lwipopts.h); upstream
                         * lwIP defaults both off, where this pattern is
                         * unsupported and would fail quietly.
                         */
                        int sock = esp_transport_get_socket(GLOBAL_STATE->transport);
                        if (sock >= 0) {
                            shutdown(sock, SHUT_RD);
                        }
                    }
                }
                pthread_mutex_unlock(&GLOBAL_STATE->transport_lock);
            }
        }

        //SYSTEM_notify_found_nonce(GLOBAL_STATE, nonce_diff, job_id, chain_num, asic_result->chip_id, asic_result->core_id);
        if(nonce_diff < 1){
            SYSTEM_notify_hw(GLOBAL_STATE, chain_num, asic_result->chip_id, asic_result->core_id);
        }else{
            SYSTEM_notify_found_nonce(GLOBAL_STATE, nonce_diff, job_id, chain_num, asic_result->chip_id, asic_result->core_id);           
        }

        free(job_jobid);
        free(job_en2);
    }
}

/*
 * Never created. It is declared in asic_result_task.h beside the live task and
 * nothing calls either the declaration or the definition -- main.c registers
 * ASIC_result_task only -- and the linker discards the whole function, so it
 * costs nothing in the image.
 *
 * Two things to fix before reviving it, both of which the live task above no
 * longer does: it writes GLOBAL_STATE->transport with no NULL check at all,
 * and it calls stratum_close_connection() on a failed write, which is the
 * cross-task destroy this file was changed to stop.
 */
void ASIC_ltc_result_task(void *pvParameters)
{ 
    uint32_t chain_num = ((AsicParam *)(pvParameters))->chain_num;
    GlobalState *GLOBAL_STATE = ((AsicParam *)(pvParameters))->p_global_state;
    double nonce_diff = 0.0;

    ESP_LOGI(TAG, "ASIC_result_task initialising.");

    while (1)
    {
        //task_result *asic_result = (*GLOBAL_STATE->ASIC_functions.receive_result_fn)(GLOBAL_STATE);
        task_result *asic_result = ASIC_process_work(GLOBAL_STATE, chain_num);

        if (asic_result == NULL)
        {
            continue;
        }

        uint8_t job_id = asic_result->job_id;

        /*
         * Take everything this share needs while holding valid_jobs_lock.
         *
         * active_jobs[] is rewritten by whichever task sends work, and that
         * task frees the job already in the slot before storing the new one,
         * outside this lock -- the caveat already recorded on job_pool in
         * global_state.h, which is why pool ownership is kept in its own array
         * rather than read back through these pointers. Scoring and submitting
         * a share read them anyway.
         *
         * This is not a narrow window. job_id comes back from the ASIC, and
         * the sender walks the slots in steps of 24 modulo 128, so only
         * sixteen are ever used and each is recycled every sixteen jobs. A
         * nonce arriving for a slot that is being reused is an ordinary event.
         *
         * jobid and extranonce2 are the only fields that point outside the
         * job, so they are copied and everything else is taken by value. That
         * keeps the lock off the socket write below, which must not be held
         * across a pool that has stopped reading.
         */
        pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

        bm_job * job = GLOBAL_STATE->ASIC_TASK_MODULE[chain_num].active_jobs[job_id];
        if (GLOBAL_STATE->valid_jobs[chain_num][job_id] == 0 || job == NULL)
        {
            pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);
            ESP_LOGW(TAG, "Invalid job nonce found, 0x%02X", job_id);
            
            #ifdef STATISTIC_SYSTEM_FEATURE            
            statistic_notice_hw(&(GLOBAL_STATE->STATISTIC_MODULE), 
                chain_num, asic_result->chip_id, asic_result->core_id, ASIC_get_small_core_count(GLOBAL_STATE));
            #endif
            continue;
        }

        nonce_diff = test_ltc_nonce_value(
            job, asic_result->nonce, asic_result->chip_id,
            asic_result->core_id, chain_num, job_id);
        uint32_t job_version   = job->version;
        uint32_t job_ntime     = job->ntime;
        uint32_t job_pool_diff = job->pool_diff;
        uint8_t  job_pool_id   = job->pool_id;
        char *   job_jobid     = job->jobid       ? strdup(job->jobid)       : NULL;
        char *   job_en2       = job->extranonce2 ? strdup(job->extranonce2) : NULL;

        pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock[chain_num]);

        if (job_jobid == NULL || job_en2 == NULL) {
            ESP_LOGW(TAG, "Out of memory copying job 0x%02X; share dropped", job_id);
            free(job_jobid);
            free(job_en2);
            continue;
        }


        //log the ASIC response
        ESP_LOGD(TAG, "Nonce %08" PRIX32 " diff %.1f of %ld.", asic_result->nonce, nonce_diff, 
            job_pool_diff);

        if (nonce_diff >= job_pool_diff)
        {
            char * user = GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback ? \
                GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user : GLOBAL_STATE->SYSTEM_MODULE.pool_user;

            /*submit a share without rolled version.*/
            int ret = STRATUM_V1_submit_ltc_share(
                GLOBAL_STATE->transport,
                GLOBAL_STATE->send_uid++,
                user,
                job_jobid,
                job_en2,
                job_ntime,
                asic_result->nonce
            );
            #ifdef STATISTIC_SYSTEM_FEATURE 
            SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;
            uint8_t pool_id = 0;
            if(module->is_using_fallback){
                pool_id = 1;
            }
            statistic_notice_submit_share(&(GLOBAL_STATE->STATISTIC_MODULE), pool_id, nonce_diff);    
            #endif

            if (ret < 0) {
                ESP_LOGI(TAG, "Unable to write share to transport. Closing connection. Ret: %d", ret);
                stratum_close_connection(GLOBAL_STATE);
            }
        }
        
        if(nonce_diff < 1){
            SYSTEM_notify_hw(GLOBAL_STATE, chain_num, asic_result->chip_id, asic_result->core_id);
            #ifdef STATISTIC_SYSTEM_FEATURE            
            statistic_notice_hw(&(GLOBAL_STATE->STATISTIC_MODULE), 
                chain_num, asic_result->chip_id, asic_result->core_id, ASIC_get_small_core_count(GLOBAL_STATE));
            #endif
        }else{
            SYSTEM_notify_found_nonce(GLOBAL_STATE, nonce_diff, job_id, chain_num, asic_result->chip_id, asic_result->core_id);
            #ifdef STATISTIC_SYSTEM_FEATURE
            statistic_notice_nonce(&(GLOBAL_STATE->STATISTIC_MODULE), 
                chain_num, asic_result->chip_id, asic_result->core_id, ASIC_get_small_core_count(GLOBAL_STATE));
            #endif            
        }

        free(job_jobid);
        free(job_en2);
    }
}

