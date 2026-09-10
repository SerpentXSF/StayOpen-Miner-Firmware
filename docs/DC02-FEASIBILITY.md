# Could this firmware run a DC02?

Asked after an owner shared a boot log from a Hammer DC02 — a two-ASIC scrypt
miner — running stock firmware 2.0.0. This is the assessment, written before
any work was done, so the reasoning holds whether or not it goes ahead.

Nothing here has been tested. No DC02 has ever been in front of this project.

## What the DC02 is

From the owner's log, and worth stating because it is not a BC board with a
different sticker:

| | |
|---|---|
| ASIC | **LT0051**, two of them, scrypt — not a BM1370 |
| Core | 1.40 V at 2600 MHz, 27.0 A, about 38 W at the hashboard |
| Supply | 11.7 V in, TPS546 regulator — the same part family this firmware already drives |
| Output | 133 MH/s |
| Controller | ESP32-S3 with PSRAM, LVGL display on an i80 bus, WiFi |
| Board | reports `DC02`, board version 3 |

The infrastructure is familiar. The ASIC is not.

## The blocking issue: it needs the blob back

`lt0051.c` is the only caller of `components/a/liba.a`, a prebuilt archive with
no source. Release 2.0.26 put that driver behind
`CONFIG_STAYOPEN_ASIC_LT0051`, default off, precisely so published BC images
stop carrying object code nobody can rebuild.

A DC02 build turns that option back on. Its entire dependency surface is five
functions:

```
chain_inactive        check_top_reg        set_top_reg
pack_ms_job_hashJob1  pack_ms_core_job_hashJob1
```

**So no DC02 binary can be published from this project.** Not on the releases
page, not through the web flasher. A GPL-3.0 release containing object code
with no corresponding source is the exact defect this repository documents in
the vendor's own releases, and doing it knowingly would be worse than doing it
by accident, which is what happened last time.

Source-only distribution is a different matter. The licence governs
distribution, not use: an owner who builds this for their own miner is
unencumbered, and the build already has the Kconfig switch for it.

The way out, for anyone who wants one, is to reimplement those five functions.
The surface is small and the behaviour is observable — it is UART framing to
the ASIC, so a logic capture plus the register map gets most of the way there.
That is the outcome worth wanting, and it needs hardware.

## What already exists

More than expected.

- The ASIC constants are already here. `asic.h:33-39` defines
  `DC02_LOTTO_ASIC_COUNT` and the rest, aliased from the VolcMiner Tiny set.
- `asic.c` already has `DEVICE_DC02` arms for init, send work, process work,
  set frequency, asic count and core count. They are stubbed when the Kconfig
  option is off, not deleted.
- `DEVICE_DC02` is in the model enum.
- The web interface already carries a DC02 entry in its model table.
- Stratum, dual pool, the OTA container, chart history, the settings API and
  every safety fix are algorithm-agnostic.

## What is missing

1. **No `nvs_device.c` branch.** It matches BC01, BC01-Pro, BC02, BC04, BC06
   and BC08 and falls through for DC02 — so model, voltage limits, ASIC
   difficulty and job interval are never set.
2. **No `boards/dc02.defaults`.** There are two board files and both are BC.
   This one needs the GPIO map, display wiring, ASIC UART pins and regulator
   limits.
3. **A different HAL.** The vendor builds the DC series against a `volc_hal`
   component; this fork's `bc_hal` was shaped around BC boards. How far they
   diverge is unknown, and is the largest unknown in this document.
4. **Hardware.** There isn't any.

## The vendor publishes DC02 source

`baichuan-org/DC02`, described as "DC02 lab version", updated 2026-08-21. It
contains `main/nvs_device.c`, a full `sdkconfig`, `partitions.csv`,
`config.cvs` and `components/volc_hal` — which is most of items 1 to 3 above.

Two caveats, both familiar.

It ships `components/a` as well, so the blob is theirs and unavailable there
too. And **the repository declares no licence.** It is a derivative of
ESP-Miner, which is GPL-3.0, so it ought to be — but "ought to be" is not a
grant, and the question already put to the vendor about BC04 applies here
unchanged. Read it for reference by all means; copying from it inherits an
unresolved licensing position.

## Should it be a separate repository?

**Recommendation: no. Same repository, no published binaries.**

A separate repository feels like it isolates the licensing problem, and it does
not. The blob is already committed here in `components/a`, so a new repository
would either duplicate it or depend on this one. Splitting also means porting
every fix twice across code that is about 95% shared: one stratum client, one
web interface, one OTA path, one set of safety fixes. The two would drift, and
the smaller one would rot.

What protects the claim that matters — *published binaries are blob-free* — is
not a repository boundary. It is not publishing DC02 binaries. That is a
release-channel decision and it can be made here.

The board-support mechanism for this already exists: a `boards/*.defaults` file
and a Kconfig switch. That is what they are for.

## What an owner can supply without shipping the hardware

Roughly in order of value per unit of effort.

**Easy, and worth having**

- A boot log at `DEBUG` verbosity, from power-on through to mining. The one
  already received is at `INFO` and still answered several questions.
- The full `/api/system/info` JSON — every field the firmware exposes,
  including the limits it believes in.
- Photographs of both sides of the board, in focus, especially around the
  controller module, the ASICs, the regulator and the connectors.
- Screenshots of the stock web interface, which reveal which controls the model
  claims to support.

**More effort, considerably more useful**

- The partition table: `esptool.py read_flash 0xd000 0xc00 part.bin`. Confirms
  the layout the OTA container assumes.
- An NVS dump, **sanitised** — it holds the API password and WiFi credentials.
  It also holds the exact voltage and frequency limits, which are the numbers
  that should not be guessed.
- A log across a deliberate frequency change and a voltage change, showing what
  the firmware permits and what it refuses.

**The one that changes the outcome**

- A logic-analyser capture of the UART between the ESP32 and the LT0051 during
  chip init and the first job submission. That is what makes reimplementing the
  five blob functions tractable rather than speculative. It needs the case open
  and a probe on two pins, so it is a real ask — but it is the difference
  between a DC02 build that can be published and one that cannot.

**What nobody should send**

An unsanitised NVS dump or `config.cvs`. Both carry live credentials. The log
already received prints the device's API password in plain text at every boot,
and it is `root`.

## Suggested order, if it goes ahead

1. Read the vendor's DC02 source for reference. Resolve the licence question
   before copying anything out of it.
2. Write `boards/dc02.defaults` and the `nvs_device.c` branch from the
   published `sdkconfig`, with the numbers confirmed against an owner's NVS
   dump rather than inferred.
3. Establish how far `volc_hal` and `bc_hal` diverge. This is the step most
   likely to change the estimate and it should happen before anything is
   promised.
4. Build it. Confirm on the link map that the blob is present only in the DC02
   image and still absent from every BC image.
5. Find one owner willing to risk a board, with a stock image captured first
   and a way back. The discipline in [BC04-ACCEPTANCE.md](BC04-ACCEPTANCE.md)
   applies unchanged.
6. Source only. No release binaries, no flasher entry, until the blob is gone.

## What would stop this

- The `volc_hal` divergence turning out to be most of a port rather than a
  board file.
- No owner willing to be first on hardware nobody here can test.
- The licensing question on the vendor's DC02 source going unanswered, if
  anything is to be copied from it rather than merely read.

None of those are reasons not to look. They are reasons not to promise.
