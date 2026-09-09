# Hardware safety

What in this firmware stands between a fault and a damaged board, how each of
those protections has actually been tested, and the ones that were found not
to work.

This exists because the question was asked directly: if somebody flashes this
and their miner dies, that is on us. What follows is the honest answer, kept
current rather than written once. Where a protection is unverified, it says
so.

Companion documents: [SECURITY.md](SECURITY.md) covers information security --
authentication, OTA integrity, shipped credentials. [KNOWN-ISSUES.md](KNOWN-ISSUES.md)
carries the full history of each defect. This file is the safety-relevant
subset in one place.

---

## 1. What protects the hardware

| Protection | Where | Trigger | Response |
|---|---|---|---|
| Hashboard over-temperature | `health_maintennance.c` | board temp > `MAX_HASHBOARD_TEMP` (71 C) | power off, fan 100%, wait 100 s, restart |
| Control-board over-temperature | `health_maintennance.c` | ESP32 temp > `MAX_ENV_TEMP` (55 C) | same |
| **Unreadable temperature** | `health_maintennance.c` | 3 consecutive failed sensor reads | same |
| ~~Fan fault~~ | `health_maintennance.c` | **cannot fire -- see below** | none |
| Core voltage range | `TPS546_set_vout()` | request outside `[VOUT_MIN, VOUT_MAX]` | refused, error returned, nothing written |
| Core over-voltage | TPS546 hardware | vendor NVM `VOUT_OV_FAULT_LIMIT` | regulator's own fault response -- **this firmware never programs or reads it; see below** |

The working software paths converge on `miner_protection_handler()`: cut the
core rail, fan to 100%, hold 100 seconds, restart.

### The fan-fault protection did not work (rewritten in 2.0.28)

**Correction, 2026-09-04, found by review.** This row previously read like the
others. It should not have.

The trigger is `if(fan_error_counter ++ > 5 && force_fan_check)`
(`health_maintennance.c:221`). `force_fan_check` is a local declared `false` at
line 128 and **never assigned anywhere in the tree** -- those two lines are its
only references. The condition is therefore permanently false and **a fan fault
is never acted on.** A miner whose fan has stopped or seized will keep hashing
until the over-temperature trip catches it, if it catches it.

Two things follow. First: this is the second protection this document has
claimed that the code does not provide -- the other was `VOUT_OV_FAULT_LIMIT`,
corrected below. Both were found by review rather than by testing, because
neither trigger has ever been reached on hardware here, and a protection whose
trigger is never reached looks identical to one that works.

Second, and worth stating plainly for anyone relying on this firmware: **the
only thermal protection that is known to work is the over-temperature trip and
the unreadable-temperature trip.** The fan-fault path is not a backstop for
them. It is not a backstop for anything.

Left as inherited rather than fixed in this release: making the condition fire
means deciding what `force_fan_check` was meant to gate, and shipping a fan
trip that has never run on hardware carries its own risk of shutting down
working miners. It is recorded here, honestly, rather than quietly enabled.

### The voltage path is sound

`VCORE_set_voltage()` goes through `TPS546_set_vout()`
(`components/bc_hal/TPS546.c:1209`), which rejects any value outside
`[VOUT_MIN, VOUT_MAX]` and returns an error rather than writing it. That part
holds.

**Correction, 2026-09-04.** An earlier version of this section said the
regulator "additionally has `VOUT_OV_FAULT_LIMIT` programmed at init, so an
over-voltage would have to defeat both a software clamp and a hardware
limit." **That was wrong, and it was wrong in the worst place -- a safety
document asserting a protection that does not exist.**

The code that writes `VOUT_OV_FAULT_LIMIT` is at `TPS546.c:538`, inside a
`#if 0` block spanning lines 513-561. `TPS546_write_entire_config()`, which
also writes it, is called only from inside another `#if 0` at line 510. The
live init branch begins at line 571 and writes exactly five registers:
`ON_OFF_CONFIG`, `OPERATION`, `VOUT_SCALE_LOOP`, `VOUT_MAX`, `VOUT_COMMAND`.
No fault limits.

So whatever over-voltage, over-current and over-temperature limits the TPS546
enforces are **the vendor's factory NVM values**. They may well be sensible.
This firmware neither sets them nor reads them back, so nobody here knows what
they are. `TPS546_show_voltage_settings()` exists and would print them; it is
commented out at line 613. `VCORE_check_fault()` exists and would poll
`STATUS_WORD` while mining; it is commented out at
`health_maintennance.c:164`.

That last point deserves emphasis, because it undercuts a claim made elsewhere
in this project: **"no log records an over-temperature, over-current or
brownout event" is true only because nothing is looking.** The regulator's own
fault status has never been read on any board here.

### What the clamp does and does not bound

`TPS546_VOUT_MAX` is 520 on a BC04 -- 5.20 V across four series domains, or
1.30 V per chip nominal. That is the vendor's own value from
`sdkconfig.bc04-reference`, and it sits **at** the top of the BM1370's stated
0.65-1.30 V window rather than below it.

Two caveats that the earlier "no configuration value can drive an ASIC out of
its voltage range" glossed over:

* **The 480 ceiling is a boot-default clamp, not an API clamp.** Only
  `asicovervdef` is bounded to 480 (`nvs_device.c:39`). The API accepts
  `coreVoltage`, `coreNormalVoltage` and `coreOverVoltage` up to
  `asic_vol_max`, which is `CONFIG_TPS546_VOUT_MAX` = 520 on a BC04
  (`nvs_device.c:35`, `http_server.c:928`).
* **1.30 V per chip is nominal only.** In a four-chip series string each
  chip's share is set by its own current draw, so a weaker or hung die takes
  more than a quarter of the string voltage. A string-level `VOUT_MAX` cannot
  protect an individual die.

Treat 480 as the operating ceiling on a BC04. The API will accept more, and
there is no margin above it.

---

## 2. Defects found, and what they meant

### 2.1 A failed temperature read looked like a cold board (fixed 2026-09-04)

**The most serious thing found so far. Present in every release up to and
including 2.0.19.**

`TMP75_read_temperature()` reports a failed I2C read as **-60 C**.
`read_hash_board_temperature()` stored that value and returned `ESP_OK`
unconditionally, so nothing upstream could tell a dead sensor from a very cold
board. Both consumers of the number then ran the wrong way:

* **Thermal protection:** `-60 > 71` is false. Never trips.
* **Fan curve:** `-60 <= MIN_FAN_TEMP (30)` puts the fan at
  `MIN_PWM_PERCENT`, **18%**.

So a sensor or bus failure made the firmware **cool the board less, while it
kept hashing at full power, with nothing watching the temperature.** A fault
should push a miner toward safety; this pushed it the other way.

**Exposure differs by board.** On a BC04, fan RPM is read over the same I2C
bus, so a total bus loss aborted the health loop and rebooted -- crude, but it
stopped the mining. The exposure there is a *partial* failure: the TMP75 at
0x48 dead while the EMC2302 at 0x2e still answers. A **BC01 reads fan RPM from
a pulse counter**, not I2C, so nothing aborts: it would sit at 18% fan,
blind, indefinitely. **BC01 is the more exposed board.**

**Origin: inherited.** The -60 sentinel, the fan curve and the overheat
comparison all come from upstream. Stock vendor firmware very likely carries
the same behaviour. That changes nothing about the obligation -- it ships in
this firmware, so it is fixed here.

**Fix.** `TMP75_get_temperature()` reports failure.
`read_hash_board_temperature()` returns an error when a sensor does not
answer. The health loop counts consecutive failures and after three -- about
six seconds -- takes the same exit as an overheat. Three rather than one,
because a single dropped I2C transaction is not a dead sensor, and shutting a
working miner down over one is its own kind of harm. The reading is still
stored so the web interface has something to show; the return value is what
callers act on.

**Verification status: not verified on hardware.** It cannot be exercised on
the BC04 here, whose I2C bus is shorted: the whole health loop is gated behind
`interface_initalized`, which that board never sets. Proving it needs a
working board with an induced sensor failure. Until then it is reasoning and a
clean build, not evidence.

### 2.2 Ethernet was unreachable exactly when it was needed (fixed 2026-09-04)

Not a damage path, but a safety-relevant availability one.

`init_all_peripherals()` talks to the hashboard over I2C, and on a board whose
bus is dead it does not return -- measured at over four minutes on the BC04
here. Everything after that call is unreachable, including the Ethernet start.
A miner in that state with no WiFi credentials has **no management interface
at all**: no API, no web interface, nothing but USB.

Fixed with a watchdog that starts Ethernet if init has not finished in 90
seconds. It waits on `interface_initalized` rather than a bare timer, because
a W5500 that is already running when the core rail steps up stops answering
for good -- so Ethernet starts either after the hashboard is up, or after the
hashboard is known not to be coming up, and never in between.

**Verified on hardware**, on the failed BC04, with and without WiFi.

### 2.3 Switching off an access point that was never on (fixed 2026-09-04)

`wifi_softap_off()` called `ESP_ERROR_CHECK(esp_wifi_set_mode(...))`. With
`wifi_on=0` there is no WiFi stack, `esp_wifi_set_mode()` answers
`ESP_ERR_WIFI_NOT_INIT`, and `ESP_ERROR_CHECK` aborted the miner.

On an Ethernet-only board that is a reboot loop with no way in: come up, take
a lease, try to tidy away an access point that does not exist, panic, repeat.
Introduced into a reachable path by the 2.2 fix and **caught the same day by
testing that configuration** rather than assuming it worked. The underlying
abort was latent before that for any `wifi_on=0` configuration.

Both toggles now treat an absent WiFi stack as a configuration, not an error.

**Verified on hardware**, Ethernet-only on the BC04.

### 2.4 The radio came up before the supply it runs on (fixed 2026-09-04)

BC01 family only. WiFi was started at ~2 s; the USB-C PD contract was not
negotiated until ~12 s. For ten seconds the board took its largest current
step on its smallest supply, and a BC01 with a replacement LilyGO module
reset at five to ten seconds on every boot unless a second 5 V source -- the
module's own USB-C, plugged into a computer -- was there to carry it.

**The firmware made the miner require a USB connection in order to boot.**
Fixed by negotiating PD before `network_init()`. Not a damage path: the
hashboard is not powered until ~14 s and the loop reset before that.

Full write-up in [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

### 2.5 The core rail cannot be switched off once the bus is gone (open, and no longer silent)

**Not fixed. Raised by review on 2026-09-04 and worth knowing before it bites
somebody.**

On a BC04 the TPS546's on/off is PMBus-only -- `ON_OFF_CONFIG 0x1f`
(`TPS546.c:573`), with no enable GPIO (`bc04.defaults` sets
`OVERHEATE_CONTROL=255`). Every way this firmware has of de-energising the
hashboard goes over I2C.

So if the bus dies while the miner is running, the firmware has no way to turn
the core rail off. The regulator keeps regulating at 4.6-4.8 V from its own
last command, through resets and reboots, while:

* `ESP_ERROR_CHECK(read_fan_rpm())` (`health_maintennance.c:167`) panics the
  miner
* the ASIC reset line floats
* the EMC2302, if it has lost power, drops its PWM and the fan stops
* the next boot spends about 200 s in the TPS546 ID retry and aborts on
  `ESP_ERROR_CHECK(VCORE_init)`, roughly every four minutes, indefinitely

A BM1370 with its PLL locked draws near-full power even when not being fed
work. The end state is a board dissipating serious power with no fan control
and no thermal supervision, until a person notices.

This is **not** what happened to the BC04 here -- its last recorded state was
healthy and it failed while powered down. But it is a credible route to a
burnt smell on any board whose bus dies mid-run, and it should be closed.

The fix, when it is written: on bus loss while mining, drive ASIC reset low
while the line still answers, command the fan to 100% before the bus is gone,
stop feeding work, and log loudly. On a boot that finds an empty bus, assert
reset low immediately and stay up serving the API rather than reboot-looping,
so the state is visible.

**Guidance until then: a miner reporting an empty I2C scan, -60 C, or a
four-minute reboot loop should have its power removed.** The firmware cannot
shut the hashboard down on its own.

---

### 2.6 The stock power-on order runs the W5500 through the core transient

**Not a defect in this firmware -- this firmware fixes it. Recorded because
it affects every BC04 running factory software.**

In the BC04 source Hammer publishes, `main.c` calls `network_init()` at
line 293, which initialises the W5500 whenever `eth_on` is set, and calls
`init_all_peripherals()` at line 320, which powers the hashboard. So the
Ethernet controller is brought up and **left running while the core rail
steps from 0 to about 4.8 V across four series ASICs**, on every boot.

On the BC04 here that was not benign. From `eth1.log`:

```
I (6298)  NETWORK: Ethernet Got IP Address
I (10658) vcore: Set ASIC voltage = 4.80V
E (10728) w5500.mac: w5500_send_command(210): send command timeout
```

**Seventy milliseconds.** Link, lease, serving traffic -- then every SPI
transaction fails and does not recover for the rest of the session. A full
power cycle brought it back. Reproducible.

A device that stops responding on a supply transient and recovers only on a
power cycle is behaving like a part in **latch-up**: the supply sags while its
inputs are still being driven, current flows through the input protection into
the rail, a parasitic path fires, and it conducts until power is removed.
Latch-up is recoverable until it is not. When it is not, the die is left as a
low impedance from supply to ground.

That board's W5500 is now exactly that -- a short across the 3.3 V rail. Since
the TMP75, the EMC2302 and every I2C pull-up share that rail, the visible
symptom was three I2C devices vanishing at once and SDA/SCL reading low.

**What this project changed.** `network_init()` no longer starts Ethernet at
all; `main()` starts it after the hashboard is up and settled, so the
controller is initialised *after* the transient rather than run through it.
That was done to fix a functional bug -- Ethernet stopped passing traffic --
before anyone considered that the transient might be damaging the part.

**What is not established.** No log exists of this board running factory
firmware. Every capture is this fork, on a replacement controller module. The
ordering above is read from Hammer's published source and the failure was
observed here, but the two have not been connected on the same unit, and a
correlation on one board is not a mechanism. It is a question worth putting to
the vendor, which is how the warranty claim words it.

---

## 3. What a reboot loop does and does not do

Worth stating plainly, because reboot loops look alarming and the question of
whether they damage hardware is a fair one.

`network_init()` runs at `main.c:452`. `init_all_peripherals()` runs at
`main.c:499`, and `power_on_hashboard()` is inside it. **Every restart raised
from the network stage therefore happens before the ASICs or the core
regulator are ever energised.** That covers:

* `restart_with_reason("WiFi connection timeout")` -- the 90-second give-up in
  `network_wifi_connect()`
* `restart_with_reason("Network configuration timeout")`
* the 2.3 panic above

A miner cycling on any of those is not power-cycling its hashboard, not
thermally cycling its ASICs, and not stressing its VRM. It is annoying and it
must be fixed, but it is not a hardware-damage path.

**Reboots that happen *after* `interface_initalized` do cycle the core rail** --
a panic in the health loop, an overheat trip, an ASIC detect failure. Those are
genuine power cycles. Development and bring-up produce far more of them than
normal use: the BC04 here took eight panics in ten boots from the LEDC fan
defect before that was fixed, each one after the hashboard had been powered.
Dozens of extra power cycles is not a demonstrated damage mechanism, but it is
more cycling than a deployed miner sees, and it is worth counting rather than
dismissing.

---

## 4. Verification status

Honest summary. "Verified" means observed on hardware, not merely built.

| Protection | Status |
|---|---|
| Voltage clamp refuses out-of-range | Verified by code path; never observed firing |
| Core-voltage ceiling per model | **Added in 2.0.28.** Four API fields and the boot-mode path checked `TPS546_VOUT_MAX` rather than the vendor's cap. Untested on hardware -- it changes nothing on a BC01 |
| Hardware `VOUT_OV_FAULT_LIMIT` | **Not programmed.** The write is inside `#if 0`; the live init sets no fault limits. Whatever the TPS546 enforces is its factory NVM. See section 1 |
| Over-temperature trip at 71 C | **Not verified.** Never reached -- peak observed on a BC04 was 65 C chip / 73 C VR at 106 W |
| Unreadable-temperature trip (2.1) | **Not verified on hardware.** Needs a working board with an induced sensor failure |
| Fan-fault trip | **Rewritten in 2.0.28.** The old one could not fire at all. The replacement is verified not to fire on a healthy miner; the trip itself has never been made to fire. See section 1 |
| Ethernet stall watchdog (2.2) | Verified on a BC04 with a dead I2C bus, with and without WiFi |
| Absent-WiFi-stack tolerance (2.3) | Verified on a BC04, Ethernet only |
| PD negotiated before the radio (2.4) | Verified on a BC01: cold boot, no USB, no reboots across 30 polls |

The unverified rows are not claims that the protection works. They are
protections whose trigger has not been reached on hardware here.

---

## 4a. The pattern behind all of these

Four defects in two days, and they are the same mistake wearing different
clothes: **doing something before the thing it depends on.**

* Ethernet started before the hashboard rail had settled, so the controller
  died 70 ms into the transient
* Ethernet then deferred behind a hashboard init that, on a broken board,
  never finished -- so it never started at all
* the WiFi radio brought up before the power contract that has to carry it
* thermal protection acting on a sensor reading whose success was never
  checked
* the self test judging the fan before checking whether the board has any
  main power, then recording a hardware failure that does not exist
* the stock firmware initialising the W5500 before powering the hashboard, so
  the controller rides out a transient it should have been started after
  (section 2.6 -- inherited, and fixed here)

Each was found by testing a configuration nobody had tried: a board with a
dead bus, a miner with no WiFi credentials, a cold boot with no USB attached.
None of them show up on a healthy board on a bench with everything plugged
in, which is exactly the configuration firmware gets tested in.

The general lesson is worth more than any of the individual fixes: **when
ordering matters, say out loud what depends on what, and test the boot with
each dependency absent.**

The fifth instance is worth dwelling on, because it was found by a user rather
than by us, and because the check it needed was already written. `test_power_on()`
contains `if (*vin < 11) ret = ESP_FAIL;` -- exactly the right test, at exactly
the right threshold, placed 57 lines after the thing it should have gated. The
defect was never missing knowledge. It was ordering.

## 5. If you are running this firmware

* The thermal protections depend on the I2C temperature sensor. Up to and
  including **2.0.19**, a sensor failure disabled them silently and dropped
  the fan to 18%. If you are on 2.0.19 or earlier, that is worth knowing --
  particularly on a **BC01**, where nothing else catches it.
* Watch for a reported hashboard temperature of **-60 C**. On any release it
  means the sensor is not answering. On 2.0.19 and earlier the firmware will
  not act on it; on later builds it powers down after about six seconds.
* An observed temperature of 0 in the API means the hashboard was never
  initialised, which is different from -60 and usually means the I2C bus is
  gone entirely. `docs/BOARDS.md` covers reading an empty bus scan.
* **BC01 family, 2.0.19 or earlier:** if your miner boots only while a USB
  cable is plugged into the display module, that is the PD ordering defect in
  section 2.4, not a faulty board. It is fixed in later builds.
* **Connect the main supply before flashing.** On 2.0.20 and earlier,
  flashing a BC04 over USB with no power on the XT-30 lets the self test
  measure an unpowered fan, call it a fan failure, and record that
  permanently. The display then stays on the splash screen while Ethernet and
  the web interface work normally. Nothing is broken; see KNOWN-ISSUES.md for
  how to clear it. Fixed after 2.0.20 -- later builds say "no main power" and
  record no fault -- but connecting power first is still the right habit,
  since an unpowered board cannot be tested either way.
* **A freshly flashed board sits on its own access point.** The web flasher
  writes a blank NVS, so the miner has no WiFi credentials, waits at
  `192.168.4.1` to be configured, and never reaches the mining screen. A
  display stuck on the splash after a browser flash usually means this, not a
  fault. Restoring an NVS backup puts the previous configuration back.

---

## 5a. Connecting and disconnecting power

**Switch the supply off at its own switch or at the mains before mating or
unmating the power connector. Do not pull a live connector.**

This is ordinary practice for high-current DC connections and it costs
nothing, but it is worth saying explicitly for a BC04, which takes 12 V on an
XT-30 with no pre-charge pin and no make-break sequencing. A board mining at
around 100 W is drawing over 8 amps; separating a bare bullet connector at
that current arcs, and mating one onto discharged input capacitance draws a
large inrush that rings the input rail. Neither is good for the hardware, and
the mating direction is the harsher of the two.

One BC04 here failed permanently with an electrical smell at the moment its
XT-30 was disconnected, after two weeks of clean operation at well under its
rated power. That is one board and a correlation, not a demonstrated
mechanism -- but the mitigation is free, so there is no reason not to take it.

### An open question for the vendor

**Does the BC04 have input transient protection across the XT-30 -- a TVS
diode, a clamp, or inrush limiting?**

We cannot answer this from firmware or from logs; it needs the schematic or a
look at the board, and neither is available to this project. It is recorded
here because the answer matters and is not ours to give:

* if there **is** protection, an input transient is a poor explanation for
  that failure and the search should go elsewhere
* if there is **not**, a board that can be damaged by normal handling of its
  own power connector has a design vulnerability, and that is a different
  conversation from user error

The question has been raised with the vendor as part of the warranty claim.
Nothing here asserts an answer.

## 6. Reporting

Hardware-safety defects are treated as higher priority than features and are
disclosed here whether or not they originated in this project. If you find one,
open an issue. If it is exploitable rather than merely dangerous, see
[SECURITY.md](SECURITY.md) first.
