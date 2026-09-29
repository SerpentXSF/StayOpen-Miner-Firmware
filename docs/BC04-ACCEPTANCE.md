# What to check when a BC04 is available

Everything in this list is code that has shipped, or is waiting to ship,
without a BC04 ever having run it. The BC01 cannot stand in for any of it: it
has one ASIC instead of four, a USB-PD supply instead of XT-30, a different
regulator ceiling, and it hides several of the interface controls involved.

Work down it in order — the early items gate the later ones.

## Before anything else

Do not flash the replacement board immediately. Record what it does on the
firmware it arrives with, so there is a baseline that is not ours:

- [ ] Photograph the label and record the serial.
- [ ] Power it up on stock firmware. Capture the kernel log to a file.
- [ ] Note: does the I²C bus scan find all devices? Is there a temperature
      reading? Does the fan report RPM? Does it mine?
- [ ] Keep that log. If anything goes wrong later, it is the only evidence of
      what the board was like before we touched it.

### What the replacement actually arrived running (2026-09-11)

Worth knowing before working through the rest of this, because it is not the
firmware the list was written against.

`VER: 3.0.2 20260802`, announcing itself as "Initializing THOR system". It is
a rewrite, not a newer build of what this fork came from: `bmlib` for the
ASIC, `thermal_pid`, `thor_cfg`, `net_w5500`, `WIFI_MANAGER`, `STORAGE`, tasks
named `ASIC_RX` / `ASIC_TX` / `ASIC_WORK` / `SYS_MON`, and an embedded
`web.tar` in place of a SPIFFS `www` partition. Its web bundle calls
`/api/claw/*` — an LLM, WeChat login, Lua modules, file upload — none of which
answered when probed over HTTP.

Three things follow:

- **Do not assume our OTA container applies.** It expects the old partition
  layout and a separate www image.
### Firmware 3.0.5 moved the profiles, and ships below its own top one

A third BC04 arrived 2026-09-24 on **3.0.5 20260909**, SN `ALBC04ACA7041FD1F8`,
and its profile table is not the one the older boards had:

| | older firmware | 3.0.5 |
|---|---|---|
| Normal | 640 MHz @ 460 | **600 MHz @ 450** |
| Over-frequency | 750 MHz @ 470 | **750 MHz @ 480** |
| Customize | -- | **750 MHz @ 460** |

It arrived set to **Customize, `boot_mode` 2**, at 750 MHz / 460 -- measured
4.590 V, 6.36 TH/s, 97.5 W, 21.4 A, board 59.6 °C, regulator 67.8 °C.

**That was not a factory setting.** The unit arrived dusty, with clear signs
of having been run before. So "Customize 750 / 460" is whoever had it last,
not the vendor shipping de-rated -- an earlier draft of this section read it
the second way and was wrong. A used replacement tells you nothing about what
leaves the factory, and its operating history is unknown, which is worth
holding onto if this board also fails.

What does still follow from the firmware itself: the over-frequency profile on
3.0.5 sits **at** our 480 ceiling rather than below it, so "the vendor never
goes that high" is not true of this firmware. That comes from the profile
table the miner reports, not from how this particular unit was set.

### THOR serves a `/v2` API, and it is usable

Found by watching the miner's own web UI rather than probing it:

```
GET /v2/device/info     device_model, firmware_version, serial_number,
                        detected_chips_count, runningPartition
GET /v2/device/status   uptime_seconds, cpu_temp, wifi_rssi, eth_link_up, ip
GET /v2/miner/status    current_hashrate (H/s), temp_board, temp_vcore,
                        core_voltage_actual (V), input_voltage, power,
                        frequency, boot_mode, the three profiles, share and
                        error counters
GET /v2/miner/hashrate?range=15m
```

No authentication on any of them. `detected_chips_count` is the field that
matters for the failure this project keeps seeing: it is the vendor's own
count of the hashboard, and it going to zero is the event worth catching.
`tools/soak_monitor.py` speaks this and normalises it to our units.

- **Older THOR ran the board at 4.65 V, not 4.80.** 760 MHz, 19.4 A, 90 W, about
  5.3 TH/s, board 51 °C and regulator 58 °C, with no rejects and no hardware
  errors. Our 480 ceiling is above what the vendor now uses.
- **The boot order is unchanged.** `net_w5500` initialises at t=2710 and
  `Enabling VCORE` lands at t=10890, so the rewrite still stands the W5500 in
  front of the core rail transient.

One observation to carry into any fan work: it reports `Fan0: 0 RPM |
Fan1: 3688 RPM`. Either one fan is fitted or one tachometer is not wired.

### Baseline captured 2026-09-12: seven hours on stock, healthy

A cold boot ("Power on reset") left running overnight on WiFi, 81 telemetry
samples across 6.9 hours:

| | min | max | avg |
|---|---|---|---|
| Hashrate | 5070.9 | 5951.8 | 5746.8 GH/s |
| Iout | 19.0 | 19.7 | 19.3 A |
| Power | 88.1 | 91.1 | 89.3 W |
| Board temp | 49.4 | 54.7 | 51.5 °C |
| Regulator temp | 57.0 | 62.0 | 58.7 °C |
| Fan1 | 3555 | 3766 | 3683 RPM |

**7471 shares accepted, 0 rejected, 0 hardware errors.** Four chips, 760 MHz,
4.64 V. Best difficulty 250216959. This is what a working BC04 looks like, and
it is the comparison anything we flash later has to beat or match.

`Fan0: 0 RPM` on all 81 samples — not a transient. One fan fitted, or one
tachometer unwired.

Three outbound HTTPS failures over the night, all from `btc_mkt` fetching a
price feed, none touching mining. Worth knowing this firmware calls out to an
external service on its own.

### The hashboard is energised within ~100 ms of getting an IP

Not a fixed delay from boot. Across two cold starts whose association times
differ by three and a half seconds:

| | got IP | `Enabling VCORE` | delta |
|---|---|---|---|
| 2026-09-11 | t=10740 | t=10890 | 150 ms |
| 2026-09-12 | t=7277 | t=7367 | 90 ms |

VCORE tracks the network event, not the clock. That matters for the test
below: when Ethernet is the network path rather than WiFi, the W5500 will be
linked, addressed and seconds out of DHCP at the instant the core rail steps
up — the most active state it can be in, which is also the state the board
that died was in.

> **Before the module is swapped, work through
> [BC04-STOCK-SERIAL-SESSION.md](BC04-STOCK-SERIAL-SESSION.md).** It covers
> what can only be collected from a working board on its original firmware
> with a serial console attached — the linked-W5500 test, the eFuse state, and
> the module that is the only real way back to stock. This page picks up after
> that.

### The one measurement worth the most, and only stock firmware can take it

The last board's W5500 stopped answering about 70 ms after the core rail came
up, every time, and eventually failed short across the 3.3 V rail. The boot
order that stands the W5500 in front of that transient is the vendor's own --
`network_init` at `main.c:293`, `init_all_peripherals` at `main.c:320` in their
published source -- so a factory BC04 should do it too. Nobody has ever
recorded it happening on factory firmware, because by the time the last board
ran stock it was already dead.

**Do this before flashing anything.**

- [ ] Plug in Ethernet. Let the board boot on the firmware it arrived with and
      reach the network — link up, an address, ideally a pool.
- [ ] Watch for the moment the hashboard is energised, and whether Ethernet
      survives it. Capture the serial log across that transition; the interface
      keeps its IP either way, so the log matters more than the link light.
- [ ] Note whether Ethernet still passes traffic afterwards: ping it, load the
      interface over the cable, watch for the miner losing its pool.
- [ ] Repeat two or three times, and also once with the cable unplugged during
      boot, plugged in after the hashboard is up. If it survives that way and
      not the other, the ordering is confirmed on a second unit.

If it wedges on factory firmware, that is the same fault on a second board, on
the vendor's own code — a far stronger statement than anything that can be said
about a single unit, and the one thing the last RMA could not produce.

If it does **not** wedge, that is worth just as much: it would mean the last
board was already faulty in a way this one is not, and the ordering theory
weakens rather than strengthens. Record whichever happens.

## 1. It boots and mines at all — 2.0.28

- [ ] Flash `stay-open-bc04-*-full.bin` over USB with the web flasher.
- [ ] Bus scan finds the regulator, the fan controller and the temperature
      sensor.
- [ ] All four ASICs detected. `detected_chips_count` is 4, not 0.
- [ ] It reaches a pool and gets a share accepted.
- [ ] Input voltage, core voltage, board temperature and fan RPM all read
      something plausible — not zero.

If any of that fails, stop and compare against the stock-firmware baseline
before assuming our firmware caused it.

## 2. The core-voltage ceiling — VERIFIED 2026-09-28 on 2.0.27.1

Run on a healthy BC04 (`ALBC04ACA7041FD1F8`) mining at 640 MHz / 460, with a
control first so a refusal could not be mistaken for a broken request:

| request | HTTP | result |
|---|---|---|
| `coreVoltage=460` (legal) | 200 | applied — proves the route and method |
| `coreNormalVoltage=485` | 200 | ignored |
| `coreOverVoltage=485` | 200 | ignored |
| `asicovervdef=485` | 200 | ignored |
| `coreVoltage=485` | 200 | ignored |

485 rather than 520 deliberately: one step over the 480 cap, so a failure
would have cost 0.05 V instead of 0.40 V. The regulator measured 4.588–4.590 V
throughout and `coreVoltage` never left 460.

**Wart worth knowing:** the API answers **200 even when it refuses the value**,
so nothing tells the caller it was ignored. The firmware logs
`v0l 485 is not valid` and moves on. A settings page that showed "saved" here
would be lying.

Still untested: `asicnormalvol` set to 520 directly in NVS followed by a
restart, which is the path that does not go through this handler at all.

## 2b. The original checklist for this section

This is the one with real consequences if it is wrong, because it governs
what the firmware will command across four chips in series.

- [ ] Read the current voltage: `GET /api/system/info` → `coreVoltage`.
- [ ] Try to exceed the cap:
      `PATCH /api/system {"coreVoltage": 520}`
      Expect it to be **refused**. Before this fix it was accepted and applied
      live.
- [ ] Confirm the miner did not move: read `coreVoltage` back, and check the
      log for a voltage change.
- [ ] Repeat for `coreNormalVoltage`, `coreOverVoltage` and `asicovervdef` —
      all four took the same wrong check.
- [ ] Set `asicnormalvol` in NVS to 520 and restart. The board should come up
      at 480, and log that the stored voltage was over the ceiling.

## 2c. Dual fan reporting — VERIFIED 2026-09-29 on 2.0.28

First BC04 to run the per-channel reporting. Every sample, settled:

```
fanrpm 2440   (ch0 0   ch1 2440)
```

**The fitted fan is on EMC2302 channel 1. Channel 0 is the empty header.**
That confirms on our own hardware what an owner reported: two headers, one fan
fitted.

It also settles a design decision made without a board to check it against.
`read_fan_rpm()` keeps `fan_rpm[0]` as the *higher* of the two channels rather
than raw channel 0. Had it used raw channel 0 -- the obvious reading of "report
the channels separately" -- this board would show **0 RPM** on the display and
in `fanrpm`, and `fan_tach_proven[0]` would never latch, which silently
disarms the stall trip. The reasoning for keeping the maximum is in
`main/device.c`; this is the measurement behind it.

`systemError` stayed empty throughout, which is correct for a healthy board
and the first confirmation that the fault reporting does not false-positive.

Also visible on this board and worth its own look: the rail comes up at the
480 default and steps down to the configured 460 over about 100 seconds, on
every boot. The vendor's firmware sets its target directly at ~7 s. A board
configured to run gently still spends its first minute and a half at the
ceiling.

## 3. The fan trip — 2.0.28, positive case never tested

Only the *no false positive* half has been proven, on a BC01. The trip itself
has never fired.

- [ ] With the miner running and warm, stop the fan — unplug it, or block it
      so the tachometer reads zero. Do this with a hand on the power and be
      ready to pull it.
- [ ] Expect, within about ten seconds: a log line about the fan being driven
      and reporting 0 RPM, then `FAN_ERROR`, then the protection path — power
      off, fan to 100%, a hundred second wait, restart.
- [ ] Confirm the hashboard really did de-energise: input voltage goes to
      zero, board temperature falls.
- [ ] Reconnect the fan and confirm it recovers on the next boot.

If it does **not** trip, the check is still not working and the entry in
`KNOWN-ISSUES.md` needs to go back to open.

## 4. What happens when the regulator cannot be reached — 2.0.28

The BC04 has no USB-PD gate, so the firmware cannot switch the hashboard off
once I²C is gone. The change only makes that visible. There is no safe way to
stage this deliberately; watch for it if the board ever misbehaves:

- [ ] If a protection path ever runs while the bus is down, the log should say
      `THE HASHBOARD MAY STILL BE POWERED` and ask for the plug to be pulled —
      instead of reporting a successful power-off.

## 5. Pool B extranonce subscribe — 2.0.27, control never seen

The toggle renders only on boards reporting extranonce support, which is the
BC04. It has been verified on the wire from a BC01, but nobody has seen the
switch.

- [ ] Pool settings → Dual Pool. The **Extranonce Subscribe** switch should be
      present, below TLS.
- [ ] Turn it on, save, restart.
- [ ] In the log, after `pool B connected`, confirm
      `mining.extranonce.subscribe` is sent on pool B's connection.
- [ ] Turn it off, save, restart, and confirm it is *not* sent.
- [ ] Check the switch reflects the stored value after a reload — it is read
      from `poolBExtranonceSubscribe`.

## 6. Pool B TLS, and the fallback password — 2.0.27

Both were silently destructive before this release.

- [ ] Set pool B TLS on and save. Reload the page: the switch should still be
      on. Before the fix it read as off and saving wrote that back.
- [ ] Set a fallback stratum password. Open the pool page, change nothing,
      press Save. Confirm the fallback password is still what you set, and not
      the literal string `password`.

## 7. The blob is gone — 2.0.26

Already confirmed against the link map for both boards. Worth one check on the
real thing:

- [ ] `docs/ASIC-ABSTRACTION.md` claims the BM1370 path is blob-free from
      2.0.26. The board should mine normally with `lt0051.c` compiled out —
      the dispatch arms it used are stubs now. If a BC04 fails to init ASICs,
      that is the first thing to suspect.

## 8. Chart history — 2.0.25

- [ ] Dashboard chart populates from the device on a fresh page load.
- [ ] 1H / 4H / 24H all return data.
- [ ] Leave it unattended for an hour with no browser open, then load the page
      — the hour should be there.

## 9. The interface — 2.0.26

The BC04 shows controls the BC01 hides, so these have never been seen on a
board that displays them:

- [ ] The unofficial-firmware notice on the Settings page is readable.
- [ ] Warning and error banners are readable — dark text on the yellow ground,
      not invisible.
- [ ] The Quick Select placeholder on the pool page is legible.
- [ ] The two "update available" tags in the Firmware Update card are legible
      when an update exists. These have never been seen at all.

## 10. The web-UI update path

An owner reported being unable to apply updates through the web interface and
having to use USB. Never reproduced.

- [ ] Update from the web interface, not the flasher. If it fails, capture the
      error text and a serial log — that is what is missing to diagnose it.

## Still expected to be broken

Not defects in the replacement board if you see them:

- A BC04 cannot switch its own hashboard off once the I²C bus has gone. That
  is a hardware fact about this board, not a firmware bug.

## What the 2.0.28 soak actually covers

The 26 h BC04 soak ran on the tree at `18e6504`. The release is tagged one
commit later, at the fix for the kernel-log download, which was pulled forward
after an owner hit it: the download button was a plain browser navigation, so
it carried no Authorization header and the firmware refused it — to a user who
was signed in, on the page already streaming that log over an authenticated
socket.

That commit changes three files, all of them web-UI source. **No firmware
source differs between the soaked tree and the released tag**, so the
application image is the one that soaked; only `www.bin` is rebuilt. The
endpoint side needed no change and was confirmed against a running 2.0.28
miner: the same GET answers 401 with no header and 200 with one.

Worth being precise about, because "we soaked it for 26 hours" is only worth
something if it names the thing that was soaked. Anything landing after that
tag — the W5500 reset hold, the two concurrency fixes, the licence and source
offer in releases — is 2.0.29 and gets its own soak.
