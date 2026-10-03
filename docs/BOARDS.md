# Building for a particular board

One source tree, one build per board, one directory each.

```bash
python tools/build_board.py bc01
python tools/build_board.py bc04
```

Output lands in `build/bc01/` and `build/bc04/`, so the two images can never
overwrite each other and neither can be flashed by accident from a stale
directory.

`tools/build_board.py <board> flash monitor` passes any further arguments
through to `idf.py`.

## Why the boards are not separate source trees

Because the code is the same and the wiring is not. Everything that differs
between models — voltage domain, ASIC difficulty, job cadence, which chip
driver runs — is already selected at runtime from the model string in NVS, in
`NVSDevice_parse_config()` and the dispatch tables in
`components/asic/asic.c`.

Forking the tree per board would mean every fix from here on gets applied
twice, and the two copies drift until one of them is quietly wrong. The thing
that actually needs separating is the *build*, and that is what the board
files do.

## What actually differs

| | BC01 / BC01 Pro | BC04 |
|---|---|---|
| ASIC UART TX | 18 | 17 |
| ASIC UART RX | 17 | 18 |
| `CONFIG_DEVICE_MODULE` | `BC01` | `BC04` |

That is the whole of `boards/bc01.defaults` and `boards/bc04.defaults`. Each is
applied as a fragment on top of the base `sdkconfig`, which carries the tuning
that is not board specific — PSRAM, partition layout, log levels — so a board
file only ever states what that board changes.

The BC04 pin assignment comes from the vendor's own reference configuration
and has since been **confirmed on hardware** — a BC04 running this build
detects all four ASICs and mines. See [Bringing up a BC04](#bringing-up-a-bc04)
for what it does and what it does not do.

### The display is deliberately not one of the differences

Every device module that has a panel builds the same display stack, the same
theme and the same set of screens. `main/CMakeLists.txt` lists them —
`lotto`, `DC02`, `DC04`, `BC01`, `BC02`, `BC04`, `BC06`, `BC08` — and the panel
geometry is in the shared configuration rather than in any board fragment.

So a screen added to `lvgl_screen.c` appears on every miner this firmware
supports, including boards that have no build fragment here yet. That is the
intent, and it is worth keeping: **do not put a board test around a screen.**
If a future panel differs in size the right answer is to make the layout
respond to the panel, not to fence the screen off behind a model name.

## The trap this exists to close

The two lines above are swapped between the boards, and getting them wrong
does not look like getting them wrong. The miner boots, serves its web
interface, reports the correct chip, warms up — and never returns a single
share. It reads as dead hardware, and diagnosing it from that end took a long
time.

Two things made this easy to hit:

- **The pins are fixed when the image is built; the board model is read from
  NVS at runtime.** Nothing connected the two, so a BC01 image whose settings
  said BC04 would take every BC04 code path over BC01 wiring.
- **The Kconfig defaults in `components/bc_hal` are 17/18 — the BC04
  arrangement.** The BC01 values lived only in the generated `sdkconfig`
  checked into the repository. Deleting or regenerating that file silently
  produced a BC04 pinout for a BC01 build.

`boards/*.defaults` fixes the second. The check in `main.c` fixes the first: at
boot the firmware compares the model in NVS against the pins it was compiled
with and, where it knows that board's pinout, says so plainly rather than
letting it look like a hardware fault.

```
E serpentx: ================ BOARD MISMATCH ================
E serpentx: Settings say this is a BC01.
E serpentx: This image drives the ASIC on TX 17 / RX 18;
E serpentx: a BC01 needs TX 18 / RX 17.
E serpentx: The chip will be detected and will never return a
E serpentx: share. Flash the BC01 build instead.
E serpentx: ================================================
```

`/api/system/info` reports the same as `boardMismatch`, and the display shows

```
Wrong firmware
for this board
```

The screen matters more than it looks. A mismatched board produces exactly the
symptom the firmware already had a message for -- the chip is found, clocked,
and silent -- so it used to show **No ASIC response** and then restart, having
waited three minutes. Rebooting cannot change which pins an image was compiled
for, so it failed again three minutes later, forever, taking the web interface
needed to flash the right build away every time. A known mismatch now says what
it is, immediately, and does not restart.

Only boards whose pinout is actually known are checked — the BC01 because it
was measured on hardware, the BC04 from the vendor reference. An unrecognised
model passes, because a guess there would block a board that works.

Verified by flashing a BC04 image onto a BC01: the message appeared at boot,
`boardMismatch` came back true, and the hashrate stayed at zero — the original
failure, reproduced deliberately and now announced instead of hidden.

## Bringing up a BC04

**A BC04 has now been run with this firmware.** It passes its self test,
detects four BM1370s, and mines — measured 3 September 2026 at 640 MHz /
460 cV: 26 accepted shares, none rejected, no hardware errors in 204 nonces,
85 W, chip 53 °C against a 57 °C regulator. `boards/bc04.defaults` is
confirmed correct as written.

Three things had to be fixed before it would run, and one is still open. All
of them are below.

The board file began as three settings — the model and the two UART pins —
and that was not enough. Diffing the two reference configurations turned up
eight more, and two of those would each have produced the same failure the
BC01 spent a session on: a miner that boots, serves its interface, detects the
chip, and never returns a share. Every one of those eight proved correct on
hardware; none of them was the thing that actually broke.

| | BC01 | BC04 |
|---|---|---|
| ASIC UART TX / RX | 18 / 17 | **17 / 18** |
| ASIC reset | GPIO 3 | **GPIO 1** |
| `TPS546_VOUT_MAX` | 130 | **520** |
| `ASIC_VOLTAGE` | 120 | 125 |
| Second I2C bus | SDA 11 / SCL 12 | none |
| Fan | LEDC PWM + tach | EMC2302 |
| Power | USB-PD, HUSB238A, VBUS gate | **XT-30**, 12 V, 115 W rated |

The voltage row is the one that is not obvious. A BC04 runs **four ASICs in
series**, so its core voltage is roughly four times a single domain — about 500
where a BC01 wants 120. The base configuration caps at 130 because it is a BC01,
and a BC04 built without overriding that clamps to a quarter of the voltage it
needs. Nothing reports a fault; it simply does not work.

### When the I2C scan comes up empty

An empty bus has two very different causes and the scan cannot tell them
apart: the devices are unpowered, or they are powered and not answering. So
when nothing responds, the firmware now surveys the bus pins and says which:

```
E hammer-i2c:   no devices found -- check hashboard power and wiring
W hammer-i2c:   surveying the bus pins to tell 'unpowered' from 'dead'
W hammer-i2c:   GPIO43  pu=0 pd=0  driven low externally
W hammer-i2c:   GPIO44  pu=0 pd=0  driven low externally
```

Read it as: **EXTERNAL PULL-UP** on SDA/SCL means the rail feeding those
pull-ups is up and the lines are free, so the devices themselves are dead or
absent. **floating** means no pull-ups are present at all. **held low** means
the line is being pulled below the controller's internal pull-up.

**Do not read "held low" as "the bus is shorted."** An earlier version of this
section did, and it is not sound. The survey enables the ESP32-S3's internal
pull-up, which is roughly 45 kohm. An external I2C pull-up of 2.2-10 kohm
returned to a rail sitting at 0 V forms a divider against it and reads LOW.
So "held low" is equally what a **dead pull-up rail** looks like. The survey
narrows the fault to the I2C domain; it does not tell you whether the cause is
a short or a missing supply, and it cannot.

Distinguishing the two needs a meter, not the firmware:

* measure SDA and SCL to ground with the board powered -- a hard short reads
  near 0 V, a dead-rail divider reads a few hundred millivolts
* measure the auxiliary rail that feeds those pull-ups and the I2C devices

One BC04 here reached this state after two weeks: all three devices gone at
once, 12.3 V present at the XT-30. Neither measurement above was taken before
it was packed for warranty return, which is the lesson worth carrying -- the
survey told us *where* to look and we treated it as telling us *what* had
happened.

Note also what the healthy pull-ups elsewhere in that survey do and do not
prove. GPIO10/11/13/16 are the W5500's SPI lines on the controller's own 3.3 V
rail -- the rail that is obviously up, because the ESP32 is running and
printing the survey. They say nothing about the hashboard-side supply.

**On a socketed controller, check the module before blaming the board.**
GPIO43 and GPIO44 are the ESP32-S3's UART0 pins, brought out through the
module header. If the controller is a replacement LilyGO T-Display-S3 rather
than the vendor's own, a damaged pad on the module is indistinguishable from
a fault on the base board by this survey alone. Pull the module, power the
base board, and measure SDA/SCL at the socket: a healthy rail there means the
base board is fine and the module is the fault.

Check before anything else:

1. **eFuses.** `esptool.py get_security_info`. If Secure Boot is enforced, stop
   and read [SECURE-BOOT.md](SECURE-BOOT.md).
2. **`DEVICE_BC04` in all four dispatch functions** in
   `components/asic/asic.c` — `ASIC_send_work`, `ASIC_set_version_mask`,
   `ASIC_set_frequency`, `ASIC_read_registers`. A missing `send_work` case is
   why the BC01 never hashed.
3. **Confirm hashing by accepted shares, not by wattage.** The BC01 drew the
   same ~24 W whether it was hashing or not.

Provisioning templates are per board, for the same reason the build
directories are: `config.cvs.example` for a BC01, `config.bc04.cvs.example` for
a BC04. One file edited by hand for whichever board is in front of you is how
a BC01 ends up at 0.996 V. The
[benchmark](https://github.com/SerpentXSF/StayOpen-Hashrate-Benchmark) now has
a BC04 profile and has been run against one.

### What tuning is worth on a BC04

A full efficiency sweep at 640 MHz, ten-minute windows, twenty-minute soak on
the winner:

| Core voltage | Power | J/TH measured | Hashrate | HW errors |
|---|---|---|---|---|
| 460 (shipped normal) | 77.1 W | 14.52 | 5312 GH/s (102%) | 0.00% |
| **448** | **71.6 W** | **14.01** | 5107 GH/s (98%) | 0.00% |
| 440 — rejected | ~68 W | — | 4618 GH/s (**88%**) | 0.00% |

**Expect less than a BC01 gives up.** Power falls about 7%, but delivered
hashrate falls with it, so the real-world efficiency gain is nearer 3.5%. The
BC01 found 15% for nothing; a BC04 ships closer to its optimum and there is no
equivalent free lunch.

### And what overclocking is worth

Sweeping upward from the vendor's own over-frequency point, capped at their
480 ceiling rather than the regulator's 520:

| Setting | Hashrate | Power | J/TH | Chip / VR | Fan |
|---|---|---|---|---|---|
| 640 MHz / 448 | 5081 GH/s | 71.6 W | 14.01 | 51 / 56 °C | 81% |
| **750 MHz / 470** | **6117 GH/s** | 98.6 W | 16.11 | 65 / 73 °C | **100%** |
| 775 MHz / 470 | — | — | — | **over 62 °C** | 100% |

**This board is cooling-limited, not silicon-limited.** The sweep never came
near a voltage or error limit; it stopped because the chip passed 62 °C at
775 MHz with the fan already pinned at 100% since 750. Error rate was 0.00%
throughout, and the 750 MHz soak delivered 6117 GH/s against a rated 6120.

750/470 is the vendor's own over-frequency profile, and it is also the
practical ceiling. It costs 38% more power for 20% more hashrate and leaves
6 °C between a steady 65 °C and the firmware's 71 °C cutout, with no fan left
to give -- in one room, at one ambient temperature. Treat it as the top of the
range rather than a setting to leave unattended in a warm room.

Both profiles are worth keeping: `asicnormalf`/`asicnormalvol` at 640/448 and
`asicoverf`/`asicovervol` at 750/470, with `boot_mode` selecting between them
(0 normal, 1 over-frequency). That way neither has to be re-measured to switch.

The 440 row is the one worth remembering: the chip lost **11.6% of its hashrate
at a zero error rate**. Undervolting a BM1370 past its limit does not
necessarily corrupt work, it just produces less of it, and nothing in the
hardware-error counters notices. Anything tuning by error rate alone will
choose that setting and call the board healthy.

### What a BC04 actually wants

Taken from the vendor's own `config.cvs` for this board, not guessed:

| | |
|---|---|
| `devicemodel` / `boardversion` | `BC04` / `V05` |
| Normal profile | 640 MHz @ **460** |
| Over-frequency profile | 750 MHz @ **470**, ceiling 480 |
| `flipscreen` | **1** — the opposite of a BC01 |
| Core voltage reported | ~4.79 V; four ASIC domains in series |
| Draw | **~85 W**, roughly 16–18 A on the core rail |

`selftest` is worth leaving at `0` on a first flash. `0` means *run it* —
it is set to `1` once it passes — and on this board it is a genuinely useful
check: it exercises the fan through all four PWM steps and confirms all four
ASICs before the miner ever tries to mine.

### The BC04 has Ethernet, and it is an ordering problem

This page has been wrong about this twice. It first warned that the BC04
might have no W5500 and would fail the self test's Ethernet check -- the
opposite is true on both counts. It then said the W5500 was being reset or
browned out by the core regulator, a hardware fault no software could touch.
That was also wrong.

**Ethernet works. It has to be started after the hashboard, not before.**

The symptom was real enough. A W5500 that is already running when the core
rail steps up to power the hashboard stops answering about 70 ms later and
never recovers:

```
I (10658) vcore: Set ASIC voltage = 4.80V
E (10728) w5500.mac: w5500_send_command(210): send command timeout
E        esp_eth: eth_on_state_changed(151): ethernet mac set link failed
```

The interface keeps its address and silently stops passing packets, so the
miner looks networked and cannot reach a pool. These were all eliminated by
measurement and none of them was the cause:

- **SPI clock.** 16 MHz is the W5500's rated ceiling; 8 MHz behaves identically.
- **Task starvation.** `volc_delay()` is a plain `vTaskDelay`.
- **Supply sag.** 12.30 V idle to 12.125 V under 85 W -- 1.4 %.
- **Pin or bus conflict.** Nothing else uses SPI2, and `power_on_hashboard()`
  only writes a voltage over I2C. No GPIO is touched between the two events.
- **A wedged socket.** `esp_eth_stop()` cannot even reset the PHY afterwards.

Five hypotheses eliminated is not the same as all of them, and the conclusion
drawn from that -- "therefore hardware" -- was the mistake. **Every one of
those experiments disturbed a controller that was already running.** None
tried initialising one *after* the transient. That case works, and keeps
working: a BC04 hashing at 750 MHz serves its own API over Ethernet with zero
W5500 errors.

So `network_init()` no longer starts Ethernet at all. `main()` calls
`network_eth_start()` once `init_all_peripherals()` has powered the board and
the supply has settled.

`network_eth_recover()` remains for a controller that is already wedged, and
still cannot help one: `esp_eth_start()` only reopens socket 0, never re-running
`emac_w5500_init()` which writes the MAC and sets MACRAW mode. Do not be
tempted to "improve" it by starting anyway when the stop fails -- a failed stop
means the driver never emitted `ETH_EVENT_STOP`, the netif is still attached,
and starting again trips `assert failed: netif_add (netif already added)`, a
reboot loop for as long as Ethernet is enabled. That was tried, and it cost
nine boots to learn.

**Verified both ways on hardware**, at 750 MHz with the board hashing:

| Case | Result |
|---|---|
| WiFi + Ethernet | both interfaces serve the API, 0 W5500 errors |
| **Ethernet only**, no WiFi credentials | 6218 GH/s, 50 shares, 0 rejected, 0 hardware errors, served over Ethernet |

The second is the one that matters for a freshly flashed miner, which has no
WiFi credentials. Deferral is therefore unconditional, and the published image
ships with Ethernet **on**.

`eth_on` and `wifi_on` are settable through `PATCH /api/system` and reported by
`/api/system/info`, so either can be changed without a factory restore.

## Releases

```bash
python tools/ship.py bc01 bc04 --push --github
```

That builds every board, packages it, publishes the flasher and cuts the
GitHub release, all at the version the build produced. Doing those as four
separate commands is how they drift: the flasher once served 2.0.15 while the
newest release was 2.0.14 and carried BC01 files only, so a BC04 owner reading
the releases page would have concluded the board was unsupported while the
flasher was already handing out a working image for it. `ship.py` refuses if
the boards were built at different versions, if a board has no artifacts, or
if the tag is already released.

It deliberately does not bump the version. Edit `CONFIG_APP_PROJECT_VER` in
`sdkconfig` first -- publishing different content under a version that has
already been released is what makes a version number worthless.

The individual steps still exist and still work:

```bash
python tools/build_board.py bc01
python tools/make_release.py bc01
python tools/publish_flasher.py bc01 bc04 --push
```

Artifacts carry the board in the filename (`stay-open-bc01-<version>-full.bin`),
and `make_release.py` reads that board's build directory and refuses to publish
a build whose own sdkconfig names a different model than the filename claims.
The filename is the only thing telling someone which board an image is for, and
the cost of getting it wrong is a miner that looks broken.

Only the BC01 is published. A BC04 has now been run on real hardware and
mines, but its Ethernet does not survive the hashboard powering up, so a
published image would hand BC04 owners a miner that needs WiFi without saying
so. That gets published once the Ethernet question is closed. Either model can
be built from source with the command at the top of this page.

### Why the flasher image lives on gh-pages

esp-web-tools fetches the firmware with XHR, and GitHub release assets are
served without CORS headers, so the image has to come from the same origin as
the page. It therefore has to be somewhere Pages can serve.

Keeping it on `main` meant every release added another 12 MB blob to the
project's permanent history, where nothing could remove it afterwards.
`publish_flasher.py` writes `gh-pages` as a single **orphan** commit instead, so
the previous image stops being referenced rather than accumulating, and the
branch can be regenerated wholesale at any time. It is generated output, not
source, which is why the push is a force.

The branch is assembled with git plumbing, so publishing never touches the
working tree, the index, or the current branch. `.gitignore` keeps flasher
binaries off `main` so this cannot quietly regress.

Note that history already written is not affected: the blobs from releases up
to 2.0.8 remain in `main`. Only the growth stops.
