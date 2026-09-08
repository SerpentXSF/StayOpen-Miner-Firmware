# Hammer Miner Firmware

Open-source firmware for Hammer BC-series Bitcoin miners — BC01, BC02,
BC04, BC06, and BC08 — built on the ESP32-S3.

This is a continuation of the BC04 firmware source that Chengdu Baichuan
published for Hammer under GPL obligation, restored to something that is
actually usable: correctly licensed and attributed, buildable entirely
from source, with the shipped security defects fixed and the BC01 support
that the vendor release omitted put back.

> **Licensing.** This firmware derives from
> [ESP-Miner](https://github.com/bitaxeorg/ESP-Miner), which is GPL-3.0.
> So is this. See [NOTICE.md](NOTICE.md) for attribution and
> [LICENSE](LICENSE) for terms.

> **Unofficial, and no warranty.** This firmware is written by an independent
> open-source miner. It is **not affiliated with, endorsed by, or supported
> by Hammer**, and flashing it may void your warranty with them. It comes
> with no warranty of any kind — see sections 15 and 16 of
> [LICENSE](LICENSE). **You flash at your own risk.** We are not responsible
> for damage caused by running a device beyond its limits, or by the flashing
> process leaving it unusable. Before you flash anything, read
> [docs/HARDWARE-SAFETY.md](docs/HARDWARE-SAFETY.md) — it lists what protects
> the hardware, what has actually been tested, and the two protections that
> are known not to work.

> **Security.** Stock Hammer firmware has **no authentication on any HTTP
> endpoint**. Any host on your network can read the configuration, change
> the payout address, or start a firmware update. If you own one of these
> miners, read [docs/SECURITY.md](docs/SECURITY.md) — the mitigations
> apply whether or not you ever flash this firmware.

---

## Why this repository exists

Hammer sold a commercial Bitcoin miner running firmware built on
ESP-Miner, a GPL-3.0 project, and shipped it closed-source. After what
their README calls an internal compliance audit, they published the BC04
source at [`baichuan-org/BC04`](https://github.com/baichuan-org/BC04).

That release does not finish the job:

- **No license file. No attribution.** Not a single copyright notice
  crediting ESP-Miner anywhere in the tree, though 4,367 lines are still
  identical to it and six files match byte for byte.
- **A binary blob with no source.** `components/a/liba.a` is first-party
  product code, compiled and linked into the firmware, with only a header
  published. GPL-3.0 section 6 means all of the corresponding source.
- **The BC01 release is empty.** `BC01-APP-2.0.3-20260625.zip` is 392
  bytes containing a two-line README and nothing else — while the BC01
  ships the same codebase, as its own firmware binary proves.
- **Explicitly unmaintained.** The vendor states they will provide no
  support or further development.

The evidence for each of these, and how to reproduce it, is in
[docs/PROVENANCE.md](docs/PROVENANCE.md).

None of this is asserted on the vendor's word. It is measured from the
two archives they published and from a shipping firmware binary.

## What this repository does about it

| | |
|---|---|
| **Licensing** | Full GPL-3.0 text, upstream attribution, and a documented provenance chain. |
| **Blob removal** | BM1370 builds — every BC board — no longer link the `liba.a` binary blob. The rest of it serves the LT0051 scrypt ASIC and is still required for those builds; see [docs/ASIC-ABSTRACTION.md](docs/ASIC-ABSTRACTION.md). |
| **Security** | Real authentication, the OTA error-path bug fixed, shipped credentials removed. See [docs/SECURITY.md](docs/SECURITY.md). |
| **BC01 support** | The stripped BC01 and BC02 code paths restored, and the USB-PD stage the BC04 release omitted merged in from the vendor's BC01 tree. Brings a BC01 up on hardware — see [Status](#status). |
| **Tooling** | OTA image pack/unpack, upstream diffing, provisioning. |

The first commit in this repository is the vendor's tree, unmodified.
Every change since is visible as a diff against exactly what they
released.

---

## Status

| | |
|---|---|
| Builds from source | Yes, ESP-IDF 5.5.1, no warnings from project sources |
| Web UI builds | Yes, typechecked |
| GPL compliance restored | Yes |
| OTA tooling verified | Yes — round-trips the vendor's own image byte for byte |
| BM1370 path free of the binary blob | Yes from 2.0.26. **Releases 2.0.4 to 2.0.25 were not** — see [docs/ASIC-ABSTRACTION.md](docs/ASIC-ABSTRACTION.md) |
| LT0051 path free of the binary blob | No — see [docs/ASIC-ABSTRACTION.md](docs/ASIC-ABSTRACTION.md) |
| **Run on real hardware** | **Yes**, on a BC01 with a replacement LilyGO T-Display-S3 module. Boots, negotiates USB-PD, brings up the regulator and fan, detects the BM1370, ramps to 750 MHz, and connects to a pool. See [docs/BC01-BRINGUP.md](docs/BC01-BRINGUP.md) |
| **Hashing on a BC01** | **Yes** — 1.71 TH/s at 750 MHz, 0 hardware errors, shares accepted, 24 W, 54 C. `ASIC_send_work()` had no BC01 case, so no work ever reached the ASIC; see [docs/BC01-BRINGUP.md](docs/BC01-BRINGUP.md) |

Treat this as reviewed, building, and field-tested on a BC01: it boots, negotiates USB-PD, brings up the regulator and fan, drives the BM1370, and mines.

This firmware descends from work given away by other people first —
HAN, NerdMiner, Bitaxe and ESP-Miner, and the Open Source Miners United
community. They are named in [docs/CREDITS.md](docs/CREDITS.md).

**Check your device before flashing anything.** On a retail BC01 measured
here, Secure Boot is enforced and both spare key slots are revoked, so the
stock module will only ever boot Hammer-signed firmware. eFuses are
one-way. The one-line check is in [docs/SECURE-BOOT.md](docs/SECURE-BOOT.md).

That does not put this firmware out of reach. The ESP32-S3 is a socketed,
off-the-shelf **LilyGO T-Display-S3**, and Secure Boot lives in that
module's eFuses — so a fresh module runs this firmware with no exploit
involved. The per-unit ASIC calibration sits in the hashboard EEPROM, not
on the module, so it survives the swap. It is reversible: keep the original
and the miner goes back to stock in a minute.
[docs/HARDWARE-SWAP.md](docs/HARDWARE-SWAP.md) covers it.

Either way, [docs/SECURITY.md](docs/SECURITY.md) applies — the missing
authentication affects locked-down units exactly as much as open ones.

---

## Hardware

| Model | ASICs | ASIC | Notes |
|---|---|---|---|
| BC01 | 1 | BM1370 | Single-chip, 5 nm Bitmain BM1370, ESP32-S3 |
| BC02 | 2 | BM1370 | |
| BC04 | 4 | BM1370 | |
| BC06 | 6 | BM1370 | Dual thermal sensor |
| BC08 | 8 | BM1370 | Dual thermal sensor |

Common to the family: ESP32-S3 host, TPS546 buck converter for the ASIC
core domain, TMP75 I²C thermal sensing, EMC2302 fan control, and an LVGL
display. An LT0051 scrypt ASIC path also exists in the tree.

---

## Dual-pool mining

Two pools connected at once, with the ASIC's time split between them. It
divides your hashrate rather than adding to it, and both pools must be
SHA-256d. Off unless you turn it on and configure a second pool.

Share counts will not match the ratio you set: each pool runs its own vardiff,
so the same work produces different share counts. The dashboard prints each
pool's difficulty beside its counts for that reason.
[docs/DUAL-POOL.md](docs/DUAL-POOL.md) covers the setup, how the split is
measured honestly, and the clean_jobs bug that used to discard the other pool's
shares.

## Which device

**The published builds are for the Hammer BC01 only.**

The board model is fixed at compile time and the boards do not share a pinout.
BC01 and BC04 have their ASIC UART lines **swapped** -- TX 18 / RX 17 against
TX 17 / RX 18 -- so a BC01 image on a BC04 crosses transmit and receive. The
miner boots, the web UI works, the ASIC is even detected, and no share is ever
returned. It reads as dead hardware and it is not.

| Model | Source | Published build |
|---|---|---|
| BC01 | Yes | **Yes**, verified on hardware |
| BC02, BC04, BC06, BC08 | Yes | No -- build from source, and see `sdkconfig.bc04-reference` |

Every release is named for its board, and so is every image inside it
(`stay-open-bc01-...`). The device model is also written into the settings
partition, so a miner flashed with the BC01 image reports `DeviceModel: BC01`.

## Installing

Three routes, in order of how little you need on your machine.

| | |
|---|---|
| **Browser** | [Web flasher](https://serpentxsf.github.io/StayOpen-Miner-Firmware/flasher/) — Chrome or Edge on a desktop, nothing to install |
| **esptool** | [Latest release](https://github.com/SerpentXSF/StayOpen-Miner-Firmware/releases/latest) — full, app-only, web-UI-only and OTA images, with `SHA256SUMS` |
| **From source** | [Building](#building) below, ESP-IDF 5.5.1 |

**Read [the eFuse warning](#status) before flashing anything.** On a retail BC01
with Secure Boot burned, this firmware will not boot and cannot be recovered.

The published images carry **no credentials**. Their settings partition is
generated from `config.cvs.example`, so a freshly flashed miner starts its own
access point and waits for your network, your pool and your own payout address.
Nothing mines to anyone else. `tools/make_release.py` builds the artifacts and
refuses to run if that template has been edited to hold a real address.

### Updating a miner already running this firmware

Open the miner's web interface, go to **Settings**, and upload one of these.
Your settings are kept — pool, WiFi, password, voltage, frequency, all of it.

| File | Updates |
|---|---|
| `stay-open-bc01-<version>-ota.bin` | the firmware |
| `stay-open-bc01-<version>-www-ota.bin` | the web interface only |

You do not have to say which is which. The page reads the first byte and routes
the file itself, and refuses anything that is neither.

Both are needed to move fully between releases; the web-UI file alone is enough
when a release only changes the interface. The raw `-app.bin` and `-www.bin` are
for a serial cable and will be rejected here — they are partition images, not
update containers.

**After an over-the-air update, the miner boots from the other application
partition.** A later serial flash of `-app.bin` to `0x20000` will then appear to
do nothing, because that is no longer the partition being booted. Rewrite
`ota_data_initial.bin` at `0x16000` in the same command to point back at it, or
flash the `-full.bin` instead.

## Building

Requires **ESP-IDF 5.5.1** or later.

```bash
git clone https://github.com/SerpentXSF/StayOpen-Miner-Firmware.git
cd StayOpen-Miner-Firmware
python tools/build_board.py bc01
```

Build through `build_board.py` rather than calling `idf.py` directly. It applies
that board's pin assignments from `boards/<board>.defaults` and builds into
`build/<board>/`, so two boards cannot overwrite each other's output. The BC01
and BC04 have their ASIC UART lines swapped, and a build with the wrong pair
boots, serves its interface, detects the chip and never returns a share — see
[docs/BOARDS.md](docs/BOARDS.md).

The firmware build runs the web UI build itself and packages the result into the
`www` partition, so the command above is all that is needed from a clean tree.

It will not do it twice, though. The web UI is an `ExternalProject` with
`BUILD_ALWAYS OFF`, so **after changing anything under `axe-os/src` you have to
rebuild it yourself** before the firmware build will pick the change up:

```bash
cd main/http_server/axe-os && npm run build
cd -  &&  python tools/build_board.py bc01
```

### Provisioning

`config.cvs` holds the settings written to the miner's NVS partition, including
the payout address it mines to. It is not in the repository; `config.cvs.example`
is the template to copy.

The firmware build does not read it and will not stop without it — only
`merge_bin.sh` does, and it refuses to run if the file is missing rather than
producing an image with nobody's address in it. Released images take their
settings from `config.cvs.example` instead, which is why a freshly flashed miner
comes up asking for your details rather than mining to whoever built it.

```bash
cp config.cvs.example config.cvs
$EDITOR config.cvs           # set stratumuser to YOUR address
./merge_bin.sh -c stayopen-miner-all.bin
```

### Flashing

> **Check your device first.** Signing an image and enforcing that
> signature are different things. If Secure Boot eFuses were burned at
> manufacture, this firmware **will not boot and cannot be recovered** —
> eFuses are permanent and USB access does not help. Ask the device:
>
> ```bash
> esptool.py --port COM3 get_security_info
> ```
>
> If `secure_boot_en` is set, do not flash. See
> [docs/SECURE-BOOT.md](docs/SECURE-BOOT.md).

Full image over USB, for a blank device or one you want to start clean:

```bash
esptool.py --chip esp32s3 write_flash 0x0 stayopen-miner-all.bin
```

Update over the network, for a working device. The container format is
unchanged, so this installs the same way vendor images do:

```bash
python tools/ota_tool.py pack build/stayopen-miner.bin -o update.bin \
    --pad 69cc74aeaf0ce683229d422f54428a54
curl -X POST --data-binary @update.bin \
     -H "Authorization: Bearer $TOKEN" \
     http://<miner>/api/system/OTA
```

Get `$TOKEN` from the login endpoint, or omit the header on a device with
no password set:

```bash
curl -s -X POST http://<miner>/api/system/login \
     -H 'Content-Type: application/json' \
     -d '{"password":"your-password"}'
```

Inspect any image, vendor or your own, before installing it:

```bash
python tools/ota_tool.py inspect update.bin
```

---

## Tuning it after you flash

[**Stay Open Hashrate Benchmark**](https://github.com/SerpentXSF/StayOpen-Hashrate-Benchmark)
finds the voltage this firmware runs best at, measuring each setting over a
twenty-minute window and confirming the winner with a soak.

```bash
python stayopen_benchmark.py <miner-ip> --password YOURPASS --mode efficiency --voltage-step 2 --soak 20
```

On the BC01 it was developed against, it took the miner from **24.8 W to
21.3 W** at the same frequency and no hardware errors — 16.61 to 13.92 J/TH.
That is one chip; run it on yours rather than copying the number.

One thing it found is worth knowing before you tune anything by hand.
Undervolting this silicon past its limit produces **no hardware errors at all**
— at 112 cV the chip lost eleven percent of its hashrate and still reported a
clean error rate. Watching errors alone will walk you straight past the cliff,
which is why the benchmark also checks delivered hashrate against what the
frequency should give.

It needs the API fields this firmware added in the release notes for `vrTemp`,
`hwErrorCount` and `noncesFound`, so flash before running it.

---

## Tools

| Tool | Purpose |
|---|---|
| [`tools/ota_tool.py`](tools/ota_tool.py) | Inspect, unpack, and build OTA update images. |
| [`tools/compare_upstream.py`](tools/compare_upstream.py) | Measure how much of this tree is still ESP-Miner. |
| [`tools/dwarf_dump.py`](tools/dwarf_dump.py) | Read the debug information left in `components/a/liba.a`. |
| [`tools/espimg.py`](tools/espimg.py) | Map an ESP32 image and cross-reference its literal pools, to find the code that uses a given string. |
| [`tools/xtensa_dis.py`](tools/xtensa_dis.py) | Small Xtensa disassembler, enough to read constants out of a driver. |

---

## Documentation

- [docs/PROVENANCE.md](docs/PROVENANCE.md) — where this code came from, with evidence
- [docs/SECURITY.md](docs/SECURITY.md) — findings, fixes, and mitigations for stock firmware
- [docs/HARDWARE-SAFETY.md](docs/HARDWARE-SAFETY.md) — what stands between a fault and a damaged board, and which of those protections are actually tested
- [docs/OTA-FORMAT.md](docs/OTA-FORMAT.md) — the update container, and why its obfuscation is not encryption
- [docs/AUTH.md](docs/AUTH.md) — how API authentication works, and what it does not cover
- [docs/SECURE-BOOT.md](docs/SECURE-BOOT.md) — check before flashing; some of it is irreversible
- [docs/HARDWARE-SWAP.md](docs/HARDWARE-SWAP.md) — running this on a locked-down retail miner, without an exploit
- [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) — understood, worked around, worth doing properly
- [docs/Hammer-Firmware-Investigation.md](docs/Hammer-Firmware-Investigation.md) — what the vendor has published, and where
- [docs/ASIC-ABSTRACTION.md](docs/ASIC-ABSTRACTION.md) — the withheld blob, and what replaced it

---

## Relationship to Hammer

None. This is an independent continuation by owners of the hardware,
not affiliated with or endorsed by Hammer or Chengdu Baichuan. It is not
related to Hammer's own later firmware — THOR OS, NORN OS, or GLOD OS —
which the vendor states were developed separately.

Use of the Hammer and BC model names is descriptive, to identify the
hardware this firmware runs on.

## Contributing

Issues and pull requests welcome. Contributions are accepted under
GPL-3.0.

If you find further upstream code that is still unattributed, please open
an issue — getting the credit right is the point of this repository.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).

Copyright of the upstream portions remains with the ESP-Miner, NerdQAxe+,
and LVGL authors.
