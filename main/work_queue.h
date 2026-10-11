#ifndef WORK_QUEUE_H
#define WORK_QUEUE_H

#include <pthread.h>
#include "mining.h"

#define QUEUE_SIZE 12

typedef struct
{
    void *buffer[QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} work_queue;

void queue_init(work_queue *queue);
void queue_enqueue(work_queue *queue, void *new_work);
void ASIC_jobs_queue_clear(work_queue *queue);
/* clear only one pool's queued jobs; see work_queue.c */
void ASIC_jobs_queue_clear_pool(work_queue *queue, uint8_t pool_id);
void *queue_dequeue(work_queue *queue);

/*
 * Dequeue, or give up after timeout_ms and return NULL. Pass 0 to poll.
 *
 * queue_dequeue() waits on not_empty forever, which makes `if (count > 0)
 * queue_dequeue()` a trap wherever another task can clear the same queue:
 * count is read without the lock, so the clear can land in between and the
 * caller blocks on a queue that is now empty and may stay empty. Both
 * secondary-pool queues are cleared from one task and drained from another,
 * so both ends of that had the bug.
 *
 * It also lets a reader stop waiting on a pool that has gone quiet, instead
 * of holding up every other pool it also feeds.
 */
void *queue_dequeue_timeout(work_queue *queue, uint32_t timeout_ms);
void queue_clear(work_queue *queue);

#endif // WORK_QUEUE_H