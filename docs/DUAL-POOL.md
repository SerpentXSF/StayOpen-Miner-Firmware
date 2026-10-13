# Multi-pool mining

Up to four Stratum sessions, all permanently connected, with the ASIC's time
sliced between them. The file is still called `DUAL-POOL.md` because that is
what it started as and README links to it; two pools is now the common case
rather than the only one.

**It splits your hashrate. It does not add any.** There is one ASIC and it does
one thing at a time. Every pool must be SHA-256d — Bitcoin, Bitcoin Cash,
DigiByte-SHA256. Scrypt and everything else is physically impossible on this
silicon.

Off unless you configure a second pool, so an existing miner behaves exactly as
it did before.

**Three and four pools need a board with four ASICs** — in practice a BC04. The
limit is in `stratum_poolb_task.c` (`QUAD_POOL_MIN_ASICS`), and the mode buttons
for Triple and Quad are disabled below it. A single-ASIC board sliced four ways
spends more time switching than hashing.

---

## Setting it up

**Pool settings.** A **Pools** selector — Single, Dual, Triple, Quad — then one
identical panel per pool after the first, each with host, user, password, TLS,
its own failover, and:

| | |
|---|---|
| **Hashrate split** | pool A's percentage. Always applies, in every mode. Default **50**. |
| **Share** | a secondary pool's weight relative to the *other secondaries*. Not a percentage, and it does not affect pool A. Shown in Triple and Quad only. |
| **Slice length** | how long each pool owns the ASIC before handing over, 100–60000 ms |

**Pool A's share is the slider, never a Share weight.** This is the part worth
reading twice, because the obvious reading is wrong.
`build_pool_weights()` in `main/tasks/create_jobs_task.c` gives pool A
`dual_ratio_a` — the Hashrate split — and then divides **only what is left**
among the configured secondaries: by their Share weights if any is non-zero,
and evenly between them if all are zero.

So with everything left at its default — slider at 50, three secondaries at
Share 0 — a four-pool miner runs:

| pool | share of slices |
|---|---|
| A | 50% |
| B, C, D | 16% each (the remaining 50 split three ways) |

**Not 25% each.** For an even four-way split you have to set the slider to 25
as well; the Share weights alone cannot take anything away from pool A.

**The selector is not itself stored, but a switch behind it is.** The Pools
buttons only add and remove panels — choosing fewer clears the ones they drop —
so from the page it feels like the hosts decide. Underneath, the page also
writes `dualEnable`, which it derives as "more than one pool". Nothing else is
stored about the mode.

That matters only if you drive the API directly, and then it matters a lot:

- **`dualEnable` must be 1**, or every secondary idles. `stratum_poolb_task`
  checks it before connecting anything and `create_jobs_task` gates the
  scheduler on it. Setting `poolBUrl` alone leaves the miner on pool A with no
  error anywhere.
- **Pool B specifically must have a host.** At boot `dual_enable` is
  `dualenable && pool[POOL_B].url non-empty`, so configuring pool C or D while
  leaving B blank disables all of them, not just B.
- It defaults to **0**, so a miner that has never been set up for several pools
  stays single-pool regardless of what else is stored.

Weights and slice length are re-read live, so the split can be tuned without a
restart. Changing a pool's host or port needs one — those are read at start-up.
A session can be ended without a restart; see *Ending a pool's session on
purpose* below.

## How it divides the work

`components/dual_pool/pool_scheduler.c` assigns each slice by error diffusion —
a Bresenham accumulator per pool — rather than cycling. Each pool accumulates
its weight every slice; the largest accumulator wins and pays back the total.
Over any run each pool's share of the slices converges on its weight, and the
pattern does not fall into lockstep with a pool's job cadence the way strict
rotation can.

Weights are clamped to 0–100 wherever they enter, including over the API. The
scheduler assembles them into a `uint8_t` array, so an unclamped 256 would wrap
to 0 and silently drop a pool the user had asked for.

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

## Every secondary pool has its own failover

Each pool after the first keeps a second endpoint of its own, stepped
independently of pool A's and of the other pools'. Set it in that pool's panel,
or over the API as `pool<X>FbUrl`, `pool<X>FbPort`, `pool<X>FbUser`,
`pool<X>FbPass` and `pool<X>FbTLS`, where `<X>` is `B`, `C` or `D`.

Without it, an outage on a secondary meant it retried a dead host forever while
every job the scheduler assigned it fell through to pool A -- the miner quietly
stopped splitting and nothing said so. `pool<X>UsingFailover` in
`/api/system/info` now reports which endpoint is live.

The state machine (`components/dual_pool/pool_failover.c`) tries the primary,
falls to the failover after three failed attempts, and probes the primary again
whenever the failover itself drops -- so it does not flap between two working
pools, and it does not strand itself on the backup when the primary returns.

Verified by pointing the primary at an unroutable address: it retried, switched
to the failover, and connected there after about 86 seconds.

## What is not done

- **A pool which is up but silent** -- accepting the connection and never
  sending work -- is not treated as a failure, because nothing distinguishes it
  from a quiet pool. Nothing else is outstanding on the failover path.
- **A down secondary's slices are not redistributed among the pools still up.**
  Fallthrough is one-way: work a secondary cannot take goes to pool A, not to
  the other secondaries. With pool C down in a four-pool setup, pool A gets
  C's share rather than B and D splitting it. Deferred deliberately -- it is a
  change to the scheduler's weighting rather than to the failover path, and
  nothing is lost; the hashrate just lands somewhere other than where the
  weights asked.
- **If pool A drops after having connected, its slices ARE lost.** Work keeps
  being built from the dead session and discarded at submit, while the counters
  show pool A producing. This is the same defect `5325e52` fixed for the
  secondaries and it is still open for pool A; see `docs/KNOWN-ISSUES.md`. At
  the default Hashrate split that is half the miner's output. A pool A that was
  never reachable since boot is handled correctly -- it is specifically the
  up-then-down case.
- **Per-ASIC pool assignment is impossible on this silicon**, not merely
  unimplemented. `BM1370_job` carries no chip address field, so a job cannot be
  directed at one ASIC. Time-slicing is the only mechanism available, which is
  why this splits across time rather than across chips.

## Checking the split directly

`/api/system/info` reports `pool<X>JobsSelected` / `pool<X>JobsServed` for each
pool in play. Selected is what the scheduler assigned; served is what was
actually built. The gap is work a pool lost for want of anything queued to
build from, which falls through to pool A.

On this miner that gap is about a dozen jobs at start-up, before each
secondary's first notify arrives, and flat afterwards -- so the residual
difference from an even split is a fixed cost at boot rather than a leak that
grows. Read these rather than inferring a shortfall from share counts against
several moving vardiffs.

Measured on a BC04 with **the Hashrate split set to 25 and each secondary's
Share set to 25** — not at the defaults, which would have given pool A half —
the interval-accurate split held within 1σ of 25% each, over 2,754 shares with
none rejected across two hours.

`pool<X>Connected`, `pool<X>SharesAccepted`, `pool<X>SharesRejected` and
`pool<X>Diff` are reported per pool as well.

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
