# Verifying the two 2.0.29 concurrency fixes on hardware

Two fixes are on `main`, both in the right-hand column of
[KNOWN-ISSUES.md](KNOWN-ISSUES.md) — unreleased, and not yet run on a board.

| commit | fix | files |
|---|---|---|
| `8cb86d3` | Dual pool kept feeding a pool that had gone away | `main/tasks/create_jobs_task.c` |
| `4d0bdf4` | A share could be scored and submitted from a freed job | `main/tasks/asic_result_task.c`, `components/asic/bm1370.c`, `components/asic/lt0051.c` |

`9aa6cd8` is the parent of the first, so it is the last tree carrying both
defects. The second test needs it.

**The two are not equally provable, and saying so first decides how much each
result is worth.** The dual-pool fix has a direct, quantitative signal in
`/api/system/info` that changes the moment the condition is induced; it can be
proven, in about three hours, with one extra machine on the LAN. The
use-after-free cannot be positively confirmed from outside the miner. Nothing
counts "shares scored from a recycled slot", and with heap poisoning off a
freed `bm_job` usually still reads correctly, so the defect was frequently
silent even while it was firing. The most that can be established from the
network is that a stated set of symptoms is absent at a stated rate over a
stated duration — and that is only a number worth reporting if the same rig is
run against `9aa6cd8` for comparison. Section 3 says how, and what that
comparison does and does not settle.

Nothing below changes core voltage, frequency, fan settings or anything that
touches a rail. The only firmware change involved is flashing a build of this
repository, twice.

## 1. The instruments

Everything here is reachable over the network. Sign in first:

```
POST /api/system/login        {"password": "..."}   -> token
GET  /api/system/info         Authorization: Bearer <token>
GET  /api/system/cores        Authorization: Bearer <token>
GET  /api/system/log/download Authorization: Bearer <token>
ws://<miner>/api/ws?token=<token>
```

Fields that matter for these two tests, all in `/api/system/info`:

| field | source | what it says |
|---|---|---|
| `poolAJobsSelected` / `poolBJobsSelected` | `jobs_selected[]` | what the slice scheduler assigned, counted once per job |
| `poolAJobsServed` / `poolBJobsServed` | `jobs_served[]` | which pool the job was actually built for |
| `poolBConnected` | `SYSTEM_MODULE.poolB_connected` | pool B session state |
| `poolBUsingFailover` | | which pool B endpoint is live |
| `poolBSharesAccepted` / `poolBSharesRejected` | | pool B's own acknowledgements |
| `sharesAccepted` / `sharesRejected` | | pool A |
| `sharesRejectedReasons` | `rejected_reason_stats[]` | the distinct reject strings and their counts, present only when there are rejects |
| `hwErrorCount`, `noncesFound` | `recveived_hw`, `recveived_nonce` | lifetime totals |
| `hwNumber`, `nonceNumber`, `hwRate` | same totals | `hwRate` is the percentage, computed on the miner |
| `freeHeap` | `esp_get_free_heap_size()` | |
| `uptimeSeconds` | | a decrease is a restart |
| `stratumDiff`, `poolBDiff` | | needed to compare the two pools at all |

`/api/system/cores` breaks the same nonce and error totals down per core
(`cores[]` with `nonces` and `errors`, plus `coresSeen`, `coresWithErrors`,
`totalNonces`). All counters are lifetime totals cleared only by a restart, so
every measurement below is a difference between two reads.

**Two properties of the log path constrain what can be done with it.**

`main.c` raises `asic_result`, `create_jobs_task`, `stratum_task` and
`stratum_api` to debug level at boot, so the mining path logs one line per
nonce the ASIC returns. `stratum_poolb` is *not* in that list, which is fine —
every line it emits for these tests is INFO or WARN.

The ring buffer behind `/api/system/log/download` is 262144 bytes
(`LOG_BUFFER_SIZE`), and each line carries a timestamp prefix added by the
vprintf hook. On a BC04 at ~6.2 TH/s with `asic_difficulty` 256 the result task
sees roughly 5.6 nonces a second, so the per-nonce line alone is about 800
bytes a second and the ring holds **on the order of five minutes**. That figure
is computed from the code and the known share rate, not measured. It means the
download is a snapshot for catching something that just happened, and nothing
longer can be reconstructed from it.

The WebSocket carries the same stream, and it is lossy on purpose:
`log_queue` is 128 entries deep and a full queue drops the line. So a
WebSocket capture is evidence that a line *appeared*, never evidence of how
many times. Anything that has to be counted must be counted from the API
totals. Keep other browser tabs on the miner closed during a run — the server
holds eight sockets and reclaims the oldest.

## 2. Dual pool no longer feeds a pool that has gone away

### 2.1 What changed and what it shows

`create_jobs_task` used to give pool B a slice whenever `poolb_notification`
was non-NULL. That variable holds pool B's last notify for the life of the
process, so it stayed true across a disconnect: the miner kept building work
for a pool it could not reach, and every nonce found against that work reached
the submit path, found `transportB` NULL, and was dropped. The test is now
`GLOBAL_STATE->transportB != NULL`, read under `transportB_lock`, and the job
falls through to pool A instead.

The counters make it visible without a log at all. `jobs_selected[chosen]` is
incremented before the connection is consulted, and `jobs_served[pool_id]`
after, so while pool B is down:

* `poolBJobsSelected` keeps rising at the configured rate
* `poolBJobsServed` **does not move at all**
* `poolAJobsServed` absorbs the difference

which gives a checkable identity over any interval while pool B is down:

```
Δ poolBJobsServed   == 0
Δ poolAJobsServed   == Δ poolAJobsSelected + Δ poolBJobsSelected
```

On the unfixed build, `poolBJobsServed` rises in step with
`poolBJobsSelected` and `poolAJobsServed` tracks only `poolAJobsSelected` —
which is exactly the reading that made the miner look like it was splitting
work as configured.

There is a second, independent signal in the log. `asic_result` runs at debug
level, so when a nonce is found against a pool B job that cannot be submitted
it prints

```
pool B share dropped: not connected
```

On the fixed build this line is expected in a short burst immediately after
the disconnect — work already queued and already in the ASIC's slots still
comes back — and then must stop. The ASIC job queue holds up to ten jobs at a
300 ms interval and the BM1370 recycles each of its sixteen live slots every
sixteen sends, so the burst should be over within about ten seconds. On the
unfixed build it continues for as long as pool B is away.

No log line is emitted when a slice is diverted. The counters are the signal,
and that is deliberate — a line per job would flood the ring.

### 2.2 The rig

The condition is *pool B connected, serving work, then gone*. Pointing pool B
at an unreachable host from boot does **not** reproduce it: `poolb_notification`
is NULL, so even the unfixed build never selects pool B and the test proves
nothing. Pool B has to have been up.

The cheapest controlled way to do that is a plain TCP relay on a machine on the
LAN, between the miner and a real pool B, which is then killed. Nothing has to
speak stratum — the relay carries a real session, so the extranonce, the
difficulty and the notifies are genuine.

```python
# relay.py <listen_port> <pool_host> <pool_port>
import socket, sys, threading
def pump(a, b):
    try:
        while True:
            d = a.recv(4096)
            if not d: break
            b.sendall(d)
    except OSError:
        pass
    finally:
        for s in (a, b):
            try: s.close()
            except OSError: pass

lp, ph, pp = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", lp)); srv.listen(4)
print("relay on", lp, "->", ph, pp, flush=True)
while True:
    c, _ = srv.accept()
    u = socket.create_connection((ph, pp))
    threading.Thread(target=pump, args=(c, u), daemon=True).start()
    threading.Thread(target=pump, args=(u, c), daemon=True).start()
```

Killing the process closes the accepted socket, the miner's
`STRATUM_V1_receive_jsonrpc_line_ctx` returns NULL, and `poolb_close()` runs:
`transportB` is cleared under its lock, `poolB_connected` goes false, and
`stratum_queueB` is emptied. The relay's port then refuses connections, so pool
B retries every five seconds (`POOLB_RETRY_DELAY_MS`) and stays down until the
relay is restarted — which is also how the test is ended, cleanly, without
touching the miner.

**Leave the pool B failover empty.** With `poolBFbUrl` set, three failed
retries send pool B to a working endpoint and the condition ends itself.

A firewall rule on the router that drops traffic to pool B's address does the
same job if a relay host is inconvenient. It is less precise — the connection
dies on a receive timeout rather than a FIN — but the states it produces are
the same.

### 2.3 Procedure

Pool A is a real pool throughout. Pool B is the relay.

1. Set pool B to the relay's address and port, pool A to a real pool, and
   `dualEnable` 1, `dualRatioA` 50, `dualSliceMs` 1000. `poolBUrl`,
   `poolBPort`, `poolBUser`, `poolBPass` and `dualEnable` take effect **on
   restart**; `dualRatioA` and `dualSliceMs` are live. Confirm `poolBFbUrl` is
   empty.

   ```
   PATCH /api/system {"poolBUrl":"192.168.x.y","poolBPort":3334,
                      "poolBUser":"...","dualEnable":1,
                      "dualRatioA":50,"dualSliceMs":1000,"poolBFbUrl":""}
   POST  /api/system/restart
   ```

   Read the settings back. The API answers 200 even when it has refused a
   value — see BC04-ACCEPTANCE.md section 2 — so a save is not evidence that
   anything was stored.

2. Start the relay. Wait for `poolBConnected` to read 1 and for the log to show
   `pool B connected to <host>:<port> (primary)`, a `pool B difficulty:` line
   and a `pool B extranonce` line. Pool B must also have **accepted at least
   one share** before the disconnect, so that the phase A numbers are known
   good and not merely plausible.

3. Phase A, pool B up, 30 minutes. Poll `/api/system/info` every 30 s and
   record the whole object. Check that both pools' served counts are rising and
   that `poolBJobsServed` is close to `poolBJobsSelected`. Take the difference
   over the phase and compute the split.

4. Kill the relay. Record the wall-clock time and the counter values at the
   last poll before and the first poll after. Watch for
   `pool B connection lost, reconnecting`, then `pool B connect failed
   <host>:<port>` repeating about every five seconds, and `poolBConnected`
   reading 0.

5. Phase B, pool B down, 60 minutes, same polling. Keep a WebSocket capture
   running from before the kill until ten minutes after it.

6. Restart the relay. Confirm pool B reconnects, `poolBConnected` returns to 1,
   and `poolBJobsServed` starts moving again.

7. Phase C, pool B up again, 30 minutes, same polling. This is what shows the
   fix does not permanently strand pool B once it has been seen down.

### 2.4 Pass and fail

Pass, all of these, over phase B:

* `Δ poolBJobsServed == 0`, exactly
* `Δ poolBJobsSelected > 0`, at roughly the phase A rate
* `Δ poolAJobsServed == Δ poolAJobsSelected + Δ poolBJobsSelected`
* pool A's accepted shares per hour in phase B is close to the *whole* board
  rather than half of it — the hashrate went somewhere
* `pool B share dropped: not connected` stops within about ten seconds of the
  disconnect and does not reappear
* phase C restores the phase A split

Fail: any movement in `poolBJobsServed` while `poolBConnected` is 0, or pool A
share throughput in phase B that stays near its phase A rate while pool B is
away — which is the original defect, half the hashrate producing nothing.

Also fail, and a different defect: `poolBJobsServed` still frozen in phase C.
That would mean the connection came back and the scheduler did not.

One caveat on the counters. `jobs_served[pool_id]` is incremented *before*
`generate_work()`, which can return without building anything — pool A if it
cannot take `global_parameter_mutex` inside two seconds, pool B if the
extranonce is not yet set. So these count decisions, not jobs enqueued. On a
healthy board the two are the same; a `Failed to get the global_parameter_mutex`
line in the log means they are not, and invalidates the interval it appears in.

### 2.5 What this cannot prove

It cannot prove the fix is right for a pool that goes away *without* the socket
closing — a network path that blackholes, a pool that stops answering while
the TCP connection stays open. `transportB` is non-NULL until
`poolb_close()` runs, so until the receive fails, the fixed build behaves
exactly like the unfixed one and keeps feeding a pool that is functionally
gone. Killing the relay produces a clean FIN. A firewall DROP is the closer
test of that case and is worth one extra hour if the setup allows it.

It says nothing about whether the *split* is correct, only about where the work
goes when one side is down. It says nothing about pool B's failover, which is
deliberately disabled for this test and was verified separately
([DUAL-POOL.md](DUAL-POOL.md)).

And it is a single-board result on one BC04. The counters are not board
specific, so a BC01 would exercise the same code, but the numbers below are
sized for a BC04's share rate.

### 2.6 Duration

About three hours of miner time — 30 minutes up, 60 down, 30 up, plus setup and
two restarts. A defensible short version is 20 minutes per phase, one hour
total; below that the phase A split has not settled enough to compare against.
The identity in 2.4 is exact rather than statistical, so it is readable within
the first few minutes — the hour exists to show it holds, and to give pool A's
share rate time to average out.

## 3. A share can no longer be scored from a freed job

### 3.1 Why this one is different

The result task read `active_jobs[job_id]` with no lock, and the task sending
work freed the job in that slot — also with no lock — before storing the new
one. The reader now takes `valid_jobs_lock`, checks the slot, scores the nonce
and copies out what the submission needs, duplicating `jobid` and
`extranonce2` and taking everything else by value, then releases the lock
before touching a socket. The senders swap the slot under the same lock and
free the displaced job afterwards.

**There is no observable that goes from "wrong" to "right" here.** Three
reasons, and they are worth stating in full because they are the whole
difficulty of this section.

Nothing counts the event. The firmware has no notion of "this share was scored
from a slot that had just been recycled", before the fix or after it. There is
no field to watch.

`CONFIG_HEAP_POISONING_DISABLED=y`. A freed block is not scrubbed; the
allocator writes its own bookkeeping into the block and leaves the rest. A
`bm_job` is a couple of hundred bytes and its two strings are small, so a read
microseconds after the free very often returns exactly the right values. The
unfixed code therefore produced a *correct* share from freed memory most of the
times it fired. An absence of symptoms is consistent both with the fix working
and with the bug never having been reached.

And the symptoms it does produce are not distinctive. A share scored against a
reused block fails its own difficulty check and is counted as a hardware error
— indistinguishable, in the counter, from a marginal core. A share whose
strings were corrupted but whose scoring passed gets submitted and refused —
indistinguishable from a stale share.

So the only honest way to say anything is to measure the same board, at the
same settings, for the same duration, on `9aa6cd8` and on `main`, and report
both numbers. Section 3.4.

### 3.2 The proxies, in order of usefulness

**`hwRate` — the best available.** `SYSTEM_notify_hw()` is called for every
nonce whose recomputed difficulty comes out below 1. A nonce scored against a
block the allocator has already handed to something else computes against
garbage and lands there essentially always. So a use-after-free that hits
reused memory shows up as a *hardware error*, not as a rejected share, and
`hwErrorCount` over `noncesFound` is the number to compare between builds. The
baselines to compare against are real: a stock BC04 ran 6.9 hours with **0
hardware errors in 7471 shares** (BC04-ACCEPTANCE.md), and this firmware has
recorded 0 hardware errors in 204 nonces at 460 cV (BOARDS.md).

There is a floor under this that both builds share, and it must be subtracted
mentally or the A/B will look like a regression. A nonce can arrive for a slot
the sender has *already legitimately refilled*; the fixed build then scores it
against the new job, which will not match, and counts a hardware error. That
happened before the fix too, whenever the swap completed before the read. The
fix removes the unsafe-read case, not the stale-nonce case. What the A/B
measures is the difference above that floor.

**`sharesRejected` and `sharesRejectedReasons`.** A corrupted `jobid` or
`extranonce2` that still scores above difficulty reaches the pool and is
refused. Expect strings along the lines of job not found, unknown job id,
stale share, or an invalid extranonce2 size; the exact wording is the pool's.
The baseline is zero — 7471 accepted and 0 rejected on stock, 50 and 0 on this
firmware over Ethernet. Any non-zero count here is worth the whole log
around it. Note the eleventh-distinct-reason overflow recorded in
KNOWN-ISSUES.md is fixed, so a pool that puts a job id in its reject reason no
longer corrupts the tally.

**`/api/system/cores`.** This is the discriminator between the two
explanations of a raised `hwRate`. Errors from a scoring-path fault are
scattered across every core that returns nonces, because the fault has nothing
to do with which core found it. A marginal core concentrates them. So
`coresWithErrors` close to `coresSeen` with a low count on each points at the
scoring path; a handful of cores holding nearly all the errors points at the
silicon, and at nothing this fix touches.

**`freeHeap`, as a regression check on the fix itself.** The fix adds two
`strdup`s per scored nonce, about eleven allocations a second on a BC04, freed
on every exit path including the two early returns. A monotonic decline in
`freeHeap` across a long run is the failure mode to watch for, and it is a
failure of the fix rather than evidence about the bug. Take the trend over
hours, not the instantaneous value.

**`Out of memory copying job 0x%02X; share dropped`.** A WARN line that exists
only in the fixed build. It means a duplication failed, which means the heap
got tight. Its appearance is a fault report about the fix.

**Crashes.** `uptimeSeconds` going backwards is a restart; the log then carries
`Reset reason: Software panic reset` and, if it was a heap fault, an
`assert failed` or `CORRUPT HEAP` line just before it.
`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, so a panic writes a dump — but
**nothing in the firmware reads the coredump partition and there is no API for
it**, so the dump is only recoverable with USB access to the board. Plan for
that: a crash during this test is the most informative thing that can happen,
and collecting it means being able to attach a cable afterwards.

**`Invalid job nonce found, 0x%02X`.** This WARN is *not* a fault signal. It
fires whenever a nonce arrives for a slot that `clean_jobs` has invalidated,
which is every new block, on both builds. Record its rate for completeness and
do not read anything into it on its own.

### 3.3 Inducing the condition

Nothing needs to be induced. The race is driven by the miner's ordinary
operation, and the arithmetic is worth writing out because it decides the run
length.

`BM1370_send_work()` advances the slot id by 24 modulo 128, so only sixteen
slots are ever used, and one send happens every `asic_job_frequency_ms` — 300
on a BC04. A given slot is therefore freed and refilled every **4.8 seconds**,
and the whole set turns over twelve times a minute. The result task reads slots
at about 5.6 nonces a second, or roughly 20,000 an hour. Multiplying the
reader's window against the per-slot recycle period gives an expected hit rate
of order tens per hour — enough that a 24-hour run on the unfixed build should
produce a few hundred events, and few enough that a one-hour run would not
distinguish anything. The order of magnitude is derived from the code, not
measured, which is the other reason the A/B is necessary.

What makes recycling most frequent is simply the highest sustained nonce rate
the board is already configured for. Leave the board at whatever the UI
permits and it already has one. **Do not raise frequency or voltage to
increase the rate** — the gain is a fraction and the exposure is a rail.

Two settings choices:

* **Dual mining off** for the primary run. Two pools add a second sender, a
  second reject stream and two moving vardiffs, and none of that is needed to
  exercise the slot swap. Add a shorter dual-enabled run afterwards if there is
  time, because the pool B submit path is the one that reads `job_pool_id` out
  of the snapshot.
* **A pool with a stable vardiff.** `hwRate` and the reject rate are both
  per-nonce ratios, and a vardiff that moves changes the denominator.

Procedure, per build:

1. Flash. Record `version` and `runningPartition` from `/api/system/info`.
2. Restart, so all counters start at zero, and record the full object as
   sample zero.
3. Run `tools/soak_monitor.py <ip> --hours 24 --interval 30`, which already
   records the fields above to CSV, and poll `/api/system/cores` alongside it.
4. Take a `/api/system/log/download` snapshot every five minutes for the first
   hour and hourly after that — the ring holds about five minutes, so a longer
   interval is a gap, not a sample.
5. Keep a WebSocket capture running throughout, accepting that it drops lines
   under load, purely so that any `Out of memory copying job`, `CORRUPT HEAP`
   or panic line is caught with its context.
6. At the end, record the final object, and `uptimeSeconds` to confirm the
   board never restarted mid-run.

### 3.4 The A/B against the pre-fix build

This is the only thing that turns the numbers in 3.2 into evidence, and it has
to be said plainly that without it a clean 24 hours on `main` shows only that
the fixed build is not obviously broken.

Run `9aa6cd8` — the parent of `8cb86d3`, and so the last tree carrying both
defects — for
the same duration, on the same board, at the same voltage, frequency and pool,
immediately before or after the `main` run. Compare `hwRate`, the reject count
and reasons, the per-core error distribution, and `freeHeap`.

**Correcting what an earlier draft of this section said:** `9aa6cd8` is *not*
the code that shipped in 2.0.28, and it is not the tree the 26-hour soak ran
on. 2.0.28 is tagged at `fa3f8cc`, three commits earlier, and `9aa6cd8` sits
on top of it carrying the W5500 reset hold, the sdkconfig range fix and a docs
change. The reset hold has never been soaked. Treating this baseline as "the
released firmware" would import an unsoaked change into the control arm and
call it the known-good one.

**It is still the right baseline, for a different reason.** What matters for
an A/B of these two fixes is the delta to `main`, and that delta is confined
to five files: `main/tasks/asic_result_task.c` and `main/tasks/create_jobs_task.c`,
`components/asic/bm1370.c` and `components/asic/lt0051.c`, and
`components/bc_hal/hal_i2c.c`. The first four are the two fixes. The fifth is
the bridge-test correction, which only runs when the I2C scan finds nothing at
all, so it cannot execute on a board that is mining. Nothing in that delta
touches a voltage, frequency, fan or rail path, and both arms carry the reset
hold equally, so it cancels.

**It is safe.** The realistic worst case is a panic and a
reboot, which de-energises the hashboard on the way, and the board is
recoverable over OTA or USB. The image goes into the other application
partition, so the running one is untouched and a rollback is a flash away.
Expect the wart already recorded in KNOWN-ISSUES.md: a large upload to either
OTA endpoint sometimes resets a BC04 mid-transfer, and retrying works.

Three things that will wreck the comparison if they are not held fixed:
voltage and frequency, because `hwRate` depends on both; the pool, because the
vardiff and the reject wording are the pool's; and ambient temperature, because
the board's error rate moves with it. Run the two halves back to back on the
same bench rather than a week apart.

There is no safe way to make the race fire *harder* in either build. Turning on
heap poisoning would make every hit produce a detectable corruption instead of
a silent one and would be the strongest possible evidence — but it is an
sdkconfig change, so it is a third build that is not either of the two being
compared, and this repository has already been caught assuming a Kconfig
default did what it said. If it is attempted, it is a separate exercise with
its own write-up, and the two-way A/B above still has to happen.

### 3.5 Pass and fail

Pass, on the `main` run:

* `hwRate` at or below the `9aa6cd8` run's, with the difference outside what
  the two runs' own variation accounts for
* `sharesRejected` zero, or non-zero only with reasons that are explained by
  something else in the log
* `freeHeap` flat across 24 hours, no monotonic decline
* no `Out of memory copying job` line
* `uptimeSeconds` monotonic for the whole run; no panic, no coredump
* `poolBSharesAccepted` non-zero in the dual-enabled run, if one is done

Fail: any decline in `freeHeap` that survives an hour of averaging, any
`Out of memory copying job`, any panic, or a `hwRate` *higher* than the
pre-fix run's. That last one is the outcome to be most careful with — the fix
adds two allocations and a mutex to the hot path, and a regression in it would
show up exactly there.

**Inconclusive, and it is a real possibility worth naming in advance:** both
runs come back with `hwRate` near zero and no rejects. That would mean the race
either did not fire in 24 hours or fired and was harmless every time, and the
correct report is that the fix is reasoned but unproven, not that it is
verified. `KNOWN-ISSUES.md` should say so in those words.

### 3.6 What this cannot prove

It cannot show the fix works. It can show that a build with the fix does not
exhibit the symptom classes a use-after-free would produce, at a measured rate,
over a measured duration, and that a build without it does or does not. The
mechanism itself — a pointer read outside the lock that guards the slot — is
established by reading the code, and that is where the confidence in this fix
comes from.

It cannot bound the race. A 24-hour clean run puts a ceiling on the rate of
*symptomatic* events and says nothing about the rate of events, because with
poisoning off most of them are silent.

**The three sites in `components/asic/lt0051.c` cannot be tested at all.**
That driver is behind `CONFIG_STAYOPEN_ASIC_LT0051`, off by default since
2.0.26, and no BC board reports an LT0051 part — the dispatch arms are stubs.
Those three changes have the same shape as the one in `bm1370.c` and are
reviewed, not verified, and there is no hardware here that could change that.

Nor does any of this cover `ASIC_ltc_result_task()`, which carries the same
corrected pattern and is only reachable on the LTC path.

### 3.7 Duration

Twenty-four hours per build, forty-eight hours in total plus two flashes.
Shorter runs are not worth doing: at 20,000 nonces an hour, a difference of a
few hundred events over a day is resolvable against a near-zero baseline, and
the same difference over an hour is a handful of events inside the noise of a
board's own error rate. The dual-enabled run afterwards can be four hours,
long enough for both pools' vardiffs to settle and for pool B to accept shares
against snapshotted job ids.

## 4. What to record either way

Both tests produce a CSV and a set of log snapshots. Keep them with the board's
serial and the exact commit, and write the outcome up in `KNOWN-ISSUES.md`
against the entry it belongs to, including the case where it came back
inconclusive. The right-hand column of that file says these are unverified;
moving an entry out of it needs a number, and "ran for a day and nothing
happened" is a number for the dual-pool fix and is not one for the
use-after-free.
