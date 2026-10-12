# Dual-pool mining

Two Stratum sessions, both permanently connected, with the ASIC's time sliced
between them.

**It splits your hashrate. It does not add any.** There is one ASIC and it does
one thing at a time. Both pools must be SHA-256d — Bitcoin, Bitcoin Cash,
DigiByte-SHA256. Scrypt and everything else is physically impossible on this
silicon.

Off unless you enable it *and* configure a pool B, so an existing miner behaves
exactly as it did before.

---

## Setting it up

**Pool settings → Dual Pool.** Host, user, password, TLS, the same as the other
two tabs, plus:

| | |
|---|---|
| **Enable dual mining** | off by default |
| **Hashrate split** | percent to pool A; the rest goes to pool B |
| **Slice length** | how long each pool owns the ASIC before handing over, 100–60000 ms |

The ratio and slice length are re-read live, so they can be tuned without a
restart. Changing the pool B endpoint needs one.

## How it divides the work

`components/dual_pool/pool_scheduler.c` assigns each slice by error diffusion —
a Bresenham accumulator — rather than alternating. Over any run the fraction of
slices given to pool A converges on the ratio, and the pattern does not fall
into lockstep with a pool's job cadence the way strict alternation can.

Each job is tagged with the pool that issued it (`bm_job.pool_id`), built with
that pool's extranonce and version mask, and its nonce is submitted back to
that pool. Sending one pool's work to the other gets it rejected as an unknown
job id, and would credit the wrong place.

## Reading the result: share counts lie

Each pool runs its own vardiff, and they rarely agree. On the miner this was
developed against, pool A sat at **1024** and pool B at **8192** — and pool B
opened at **524288** before its vardiff found the hashrate.

At an even split that produces roughly eight times as many pool A shares. It
looks badly broken and is not. What the scheduler divides is ASIC *time*, and
the work delivered to a pool is

```
work  =  shares x difficulty
```

so that is what has to be compared. The dashboard prints each pool's difficulty
beside its share count (`2076 / 0 @ 1024`) for exactly this reason, and
`/api/system/info` reports `stratumDiff` and `poolBDiff`.

Measure it by accumulating *shares since the last sample* multiplied by that
pool's difficulty **at the time**. Multiplying final counts by final difficulty
gets it wrong whenever vardiff has moved, which on a fresh connection it always
has.

## A clean from one pool must not touch the other

The bug worth knowing about, because it cost real shares.

`cleanQueue()` runs when **pool A** sends `clean_jobs`. It used to clear the
whole ASIC job queue and invalidate all 128 job slots — pool B's included.
Every nonce the ASIC then returned for a pool B job was dropped in
`asic_result_task` as an unknown job id.

Pool A sends `clean_jobs` on every new block, so pool B's in-flight work was
discarded every few minutes along with any share found against it. Nearly
always that share is worth nothing. Occasionally it is worth a block.

It also skewed the split. Measured over four hours before the fix:

```
work A 2.39e6 / B 1.98e6  ->  54.6% / 45.4%     (configured 50/50)
```

Pool B was not being under-served. It was being served and then having the
results binned.

A clean now touches only the work of the pool that asked for it:

- `ASIC_jobs_queue_clear_pool()` drops one pool's queued jobs and puts the
  survivors back, instead of emptying the queue
- `cleanQueue()` and the `abandon_work` path skip slots owned by pool B
- `stratum_poolb_task` does the mirror, so pool B's own clean invalidates its
  slots and leaves pool A's alone — without that half, stale pool B jobs stayed
  valid and their nonces went to a pool that had already moved on

Slot ownership is recorded in `job_pool[]` as each slot is filled, under the
same lock as `valid_jobs`. Reading it back from `active_jobs[]` would have been
simpler and wrong: that array is written outside `valid_jobs_lock`, so
following those pointers from another task is a use-after-free waiting to
happen.

## Pool B has its own failover

Pool B keeps a second endpoint of its own, stepped independently of pool A's.
Set it in the Dual Pool tab, or over the API as `poolBFbUrl`, `poolBFbPort`,
`poolBFbUser`, `poolBFbPass` and `poolBFbTLS`.

Without it, a pool B outage meant it retried a dead host forever while every
job the scheduler assigned it fell through to pool A -- the miner quietly
stopped dual mining and nothing said so. `poolBUsingFailover` in
`/api/system/info` now reports which endpoint is live.

The state machine (`components/dual_pool/pool_failover.c`) tries the primary,
falls to the failover after three failed attempts, and probes the primary again
whenever the failover itself drops -- so it does not flap between two working
pools, and it does not strand itself on the backup when the primary returns.

Verified by pointing the primary at an unroutable address: it retried, switched
to the failover, and connected there after about 86 seconds.

## What is not done

- Nothing outstanding on the failover path. The remaining gap is that a pool
  which is up but silent -- accepting the connection and never sending work --
  is not treated as a failure, because nothing distinguishes it from a quiet
  pool.

## Checking the split directly

`/api/system/info` reports `poolAJobsSelected` / `poolAJobsServed` and the pool
B pair. Selected is what the scheduler assigned; served is what was actually
built. The gap is work one pool lost to the other for want of anything queued
to build from.

On this miner that gap is about a dozen jobs at start-up, before pool B's first
notify arrives, and flat afterwards -- so the residual difference from an even
split is a fixed cost at boot rather than a leak that grows. Read these rather
than inferring a shortfall from share counts against two moving vardiffs.

## Ending a pool's session on purpose

```
POST /api/system/pool/drop   {"pool": "C"}
```

Ends that pool's Stratum session. The pool reconnects by itself afterwards, the
same way it would after any disconnect. The response says what was recorded,
not what has happened -- the pool task acts on its next pass, measured at 0.0
to 0.6 s -- so poll `pool<X>Connected` to watch it:

```json
{"pool": "C", "wasConnected": true, "status": "drop requested"}
```

`B`, `C` or `D`, upper or lower case. Pool A is refused: its transport has no
lock and its receive timeout is ten minutes, so there is no prompt and safe way
to end its session from the HTTP task. An unconfigured pool gives 404.

**Why this exists.** The defects fixed on this path were all about what a new
session inherits from the one it replaced -- work, extranonce, a held notify --
and a session can only end from the far side. Nothing in the API could cause
one: changing a pool's host needs a restart, and a restart is two cold starts
rather than a reconnect, so it cannot exercise this at all. Confirming those
fixes meant standing a Stratum relay in front of the pool, repointing the pool
at it, opening a firewall port and restarting twice. Blocking the pool's
hostname at a router does nothing, incidentally: that is DNS filtering, and the
firmware resolved the name once at connect time and holds a socket to the
address.

It is not behind a build flag. A flag would mean the binary that gets tested is
not the binary that ships, and every fix on this path was settled by what the
firmware did rather than by reading it. It needs the API password, and the most
an authorised caller achieves is a reconnect.

Measured over eight forced reconnects across all three secondary pools:
`jobsServed` frozen for every disconnected window, a distinct extranonce from
the pool on every reconnect, work resumed 2.0-2.9 s after reconnecting, nothing
rejected, and the other pools undisturbed.

A requested drop is logged as such -- `dropping the session, asked over the
API` -- and not as the failed share write that shares its teardown path, so the
log still says why a session ended.

## Credit

Ported from the SerpentX dual-pool work for BitAxe and NerdAxe, GPL-3.0.
`components/dual_pool/` — the scheduler, the clamps, and the reentrant receive —
is imported from it; the pool B session and the hooks in `create_jobs_task` and
`asic_result_task` are written against this tree's task layout, which predates
the upstream one that work targets. See [CREDITS.md](CREDITS.md).
