#ifndef POOL_SCHEDULER_H
#define POOL_SCHEDULER_H
#include <stdint.h>
#include <stdbool.h>

/*
 * How many pools the scheduler can divide work between.
 *
 * Four, because that is what a BC04's four BM1370s are sold as and what the
 * mode selector offers. It is not a per-ASIC limit: a BM1370 job carries no
 * chip address -- see BM1370_job in components/asic/include/bm1370.h, which
 * has job_id, midstates, nonce, nbits, ntime, merkle root, previous hash and
 * version, and nowhere to say which chip should take it -- so work cannot be
 * aimed at one chip. Every pool here is served by time slicing the whole
 * chain, which is why three pools work as readily as four.
 */
#define POOL_MAX 4

typedef enum {
    POOL_A = 0,
    POOL_B = 1,
    POOL_C = 2,
    POOL_D = 3
} pool_id_t;

typedef struct {
    uint8_t   weight[POOL_MAX];  // share of slices wanted by each pool
    uint8_t   count;             // pools in play, 1..POOL_MAX
    int32_t   total;             // sum of the weights in play
    int32_t   acc[POOL_MAX];     // error-diffusion accumulator per pool
    uint16_t  interval_ms;       // slice length
    pool_id_t current;           // pool owning the current slice
    int64_t   slice_start_us;    // start time of the current slice
    bool      initialized;
} pool_scheduler_t;

/*
 * Divide slices between `count` pools in proportion to `weights`.
 *
 * The weights are relative and need not add up to anything in particular:
 * {40,30,20,10} and {4,3,2,1} schedule identically, so a user moving a slider
 * for one pool does not have to be made to rebalance the others. A count of 0,
 * or weights that are all zero, gives every slice to pool A, because a miner
 * that has been configured into a corner should keep hashing somewhere rather
 * than stop.
 */
void pool_scheduler_init_n(pool_scheduler_t *s, const uint8_t *weights,
                           uint8_t count, uint16_t interval_ms, int64_t now_us);

/*
 * Two-pool entry point: `ratio_a` percent of slices to A, the rest to B.
 *
 * Kept so the dual-mining callers need no change. The long-run split is
 * identical to the binary accumulator this replaced -- verified across
 * ratio_a 0, 25, 30, 50, 70 and 100 over ten thousand slices -- though the
 * order within a cycle differs slightly, since the two pools now each carry
 * an accumulator rather than sharing one.
 */
void pool_scheduler_init(pool_scheduler_t *s, uint8_t ratio_a,
                         uint16_t interval_ms, int64_t now_us);

/*
 * The pool that should own work right now, advancing slice boundaries as time
 * passes. Stable within a slice.
 */
pool_id_t pool_scheduler_select(pool_scheduler_t *s, int64_t now_us);

#endif // POOL_SCHEDULER_H
