#include "work_queue.h"
#include <time.h>
#include "esp_log.h"

void queue_init(work_queue *queue)
{
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
}

void queue_enqueue(work_queue *queue, void *new_work)
{
    pthread_mutex_lock(&queue->lock);

    while (queue->count == QUEUE_SIZE)
    {
        pthread_cond_wait(&queue->not_full, &queue->lock);
    }

    queue->buffer[queue->tail] = new_work;
    queue->tail = (queue->tail + 1) % QUEUE_SIZE;
    queue->count++;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->lock);
}

void *queue_dequeue(work_queue *queue)
{
    pthread_mutex_lock(&queue->lock);

    while (queue->count == 0)
    {
        pthread_cond_wait(&queue->not_empty, &queue->lock);
    }

    void *next_work = queue->buffer[queue->head];
    queue->head = (queue->head + 1) % QUEUE_SIZE;
    queue->count--;

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);

    return next_work;
}

void *queue_dequeue_timeout(work_queue *queue, uint32_t timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += timeout_ms / 1000u;
    deadline.tv_nsec += (long) (timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&queue->lock);

    while (queue->count == 0) {
        if (pthread_cond_timedwait(&queue->not_empty, &queue->lock, &deadline) != 0) {
            /* Timed out, or woken spuriously past the deadline. Either way
             * there is nothing here and the caller wants control back. */
            pthread_mutex_unlock(&queue->lock);
            return NULL;
        }
    }

    void *next_work = queue->buffer[queue->head];
    queue->head = (queue->head + 1) % QUEUE_SIZE;
    queue->count--;

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);

    return next_work;
}

void queue_clear(work_queue *queue)
{
    pthread_mutex_lock(&queue->lock);

    while (queue->count > 0)
    {
        mining_notify *next_work = queue->buffer[queue->head];
        STRATUM_V1_free_mining_notify(next_work);
        queue->head = (queue->head + 1) % QUEUE_SIZE;
        queue->count--;
    }

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);
}

/*
 * Drop only the jobs belonging to one pool, keeping the other pool's.
 *
 * A clean_jobs from one pool says nothing about the other pool's work, and
 * throwing it away loses every share already being hashed against it -- one of
 * which could be a block. Used when dual mining; the plain clear above is
 * still correct for a single pool.
 */
void ASIC_jobs_queue_clear_pool(work_queue *queue, uint8_t pool_id)
{
    void *keep[QUEUE_SIZE];
    int kept = 0;

    pthread_mutex_lock(&queue->lock);

    while (queue->count > 0)
    {
        bm_job *next_work = queue->buffer[queue->head];
        queue->head = (queue->head + 1) % QUEUE_SIZE;
        queue->count--;

        if (next_work != NULL && next_work->pool_id != pool_id) {
            keep[kept++] = next_work;      /* not this pool's to abandon */
        } else if (next_work != NULL) {
            free(next_work->jobid);
            free(next_work->extranonce2);
            free(next_work);
        }
    }

    /* put the survivors back, in order */
    queue->head = 0;
    queue->tail = 0;
    for (int i = 0; i < kept; i++) {
        queue->buffer[queue->tail] = keep[i];
        queue->tail = (queue->tail + 1) % QUEUE_SIZE;
        queue->count++;
    }

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);
}

void ASIC_jobs_queue_clear(work_queue *queue)
{
    pthread_mutex_lock(&queue->lock);

    while (queue->count > 0)
    {
        bm_job *next_work = queue->buffer[queue->head];
        free(next_work->jobid);
        free(next_work->extranonce2);
        free(next_work);
        queue->head = (queue->head + 1) % QUEUE_SIZE;
        queue->count--;
    }

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);
}
