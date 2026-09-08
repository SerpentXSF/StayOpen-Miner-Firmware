# The withheld ASIC abstraction layer

`components/a/liba.a` is a 259,738-byte precompiled Xtensa static library
that the vendor linked into the firmware while publishing everything around
it. Its only accompanying source is a header of declarations.

This documents what it contains, what has been replaced, and what has not.

---

## Status

| | |
|---|---|
| **BM1370 builds (BC01, BC02, BC04, BC06, BC08)** | **Blob-free from 2.0.26.** Not before — see below. |
| LT0051 builds (scrypt) | Still require it. See [What remains](#what-remains). |

If you are running any BC-series miner on 2.0.26 or later, this firmware
builds and runs entirely from source.

### Releases 2.0.4 to 2.0.25 were not blob-free

This section said otherwise for a year of releases, and it was wrong.

`lt0051.c` was compiled unconditionally, and it is the only caller of
`liba.a`. No BC board ever executes it: the dispatch arms it sits behind are
selected by device model, and no BC board reports an LT0051 part. But a
linker resolves references, not reachable calls. Compiling the file was
enough, and 82 sections of that archive were placed at live flash addresses
in every published BC01 and BC04 image, adding about 20 KB.

That was not a documentation slip alone. Those releases conveyed object code
with no corresponding source, which is the same defect this project raises
against the vendor. It was found by an external review of the link map in
September 2026, not by us.

From 2.0.26 the driver is behind `CONFIG_STAYOPEN_ASIC_LT0051`, off by
default, and the dispatch arms resolve to stubs when it is off. The check
that matters is the link map, not the source: after the change the map holds
one `LOAD` line for the archive and no member extracted from it, and the
image is 20,608 bytes smaller.

---

## Why it is a GPL problem

The library is not a separable System Library under GPL-3.0 section 1. It is
first-party product code, compiled from a single file and linked directly
into a GPL-licensed binary by `components/a/CMakeLists.txt`. Section 6
requires the corresponding source for the whole work, not the parts the
vendor found convenient to publish.

---

## What the vendor left in it

The blob was built with `-gdwarf-4 -ggdb` and shipped with its debug
information intact. That identifies the withheld file and its build exactly:

```
F:/workspace/work_volc/temp_mini/components/a/asic_abstraction.c
GNU C17 14.2.0 -mdynconfig=xtensa_esp32s3.so -mlongcalls
  -mdisable-hardware-atomics -gdwarf-4 -ggdb -O2 -std=gnu17
  -ffunction-sections -fdata-sections -fvisibility=hidden
```

The DWARF also carries every function signature, every local variable name,
every struct layout, and per-function line numbers in the original source.
`tools/dwarf_dump.py` extracts it.

Twenty-three functions are exported. Ten symbols are imported —
`SERIAL_send`, `SERIAL_rx`, `SERIAL_clear_buffer`, `volc_delay`, `esp_log`,
`esp_log_timestamp`, `malloc`, `free`, `memcpy`, `memset`.

The vendor also shipped **test vectors** for it, in
`components/asic/test/test-asic_abstraction.c`, with expected outputs for
each packing function. Those made the work below verifiable rather than
speculative.

---

## What has been replaced

### `CRC5`

The only blob function the BM1370 path used. Replaced by `crc5_bits()` in
[`components/asic/crc.c`](../components/asic/crc.c).

The signature is `CRC5(unsigned char *ptr, unsigned char len)` where **`len`
counts bits, not bytes**. That is not obvious from the name, and the DWARF
settles it: the function's locals are `crcin[]`, `crcout[]`, `din`, `i`,
`j`, `k` — the shift-register form of a bit-serial CRC, which only exists
because the length may not be a whole number of bytes.

The polynomial and preset match the `crc5()` already present in `crc.c`
(x⁵ + x² + 1, preset 0x1F), so `crc5_bits(p, 8n)` equals `crc5(p, n)`.

Verified against both vectors in `test-asic_abstraction.c`:

```
data 004f96e50066964f  ->  0x1f   (expected 0x1f)
data 0006004dc7030006  ->  0x0d   (expected 0x0d)
```

`bm1370.c` never called into the blob at all — its `_send_chain_inactive()`
is its own. With `CRC5` replaced, `common.c` and `eeprom.c` no longer
reference the blob either.

#### A latent bug this exposed, deliberately not fixed

`eeprom.c` calls `CRC5(data, 63)` on a 64-byte record. Because the length is
in bits, that CRC covers **63 bits — under 8 bytes — of a 512-bit record.**
Corruption anywhere past byte 8 goes undetected.

The behaviour is preserved exactly. Changing it to cover the full record
would invalidate the CRC on every already-provisioned unit in the field.
Fixing it properly needs a record-format version, which is a change for a
release that can migrate existing EEPROMs, not a silent correction.

---

## What remains

Everything else in the blob serves the **LT0051**, a scrypt ASIC, and is
reached only from `components/asic/lt0051.c`. No BC-series board uses it.

The substantial part is `pack_ms_job_hashJob1()` and
`pack_ms_core_job_hashJob1()`, ~11.5 KB of code each. They apply a bit
permutation to the 76-byte job payload through helpers the DWARF names
`scrmbl8`, `scrmbl32`, `scrmbl48` and `scrmbl616`, operating on types
`scrB8`, `scrB32`, `scrB48`, `scrB616`. The size comes from roughly 400
individually named single-bit temporaries (`b0` … `b409`) that `-O2`
flattens into straight-line shift-and-mask code.

The remaining functions are small: command packing (`pack_ms_cmd_getreg`,
`pack_ms_cmd_setreg`, `pack_ms_cmd_inactive`, `pack_ms_cmd_setaddr`),
register access wrappers (`set_top_reg`, `get_top_reg`, `check_top_reg`, and
their `_core_` equivalents), `chain_inactive`, `set_address`,
`parse_return_type`, and CRC helpers.

### Reconstructing it

The permutation is recoverable. It is a fixed bit mapping, so feeding
single-bit inputs and observing which output bit moves recovers it exactly —
608 vectors for a 76-byte payload — provided the transform is linear, which
the `b0`…`b409` structure strongly implies. That requires either an Xtensa
emulator or a symbolic reader for the shift-and-mask sequence in `.text`.

This has not been done here because no LT0051 hardware was available to
verify against, and the vendor's test vectors cover the packing functions
but not the scramble in isolation. Shipping an unverified reimplementation
of a hashing job encoder would be worse than shipping none: it would fail
silently, as rejected shares.

Contributions welcome. The test vectors in `test-asic_abstraction.c` are the
acceptance criteria, and `tools/dwarf_dump.py` gives you the structure.

---

## Tools

| Tool | Purpose |
|---|---|
| [`tools/dwarf_dump.py`](../tools/dwarf_dump.py) | Extract signatures, locals, structs and line numbers from `liba.a`. |

```bash
python tools/dwarf_dump.py components/a/liba.a          # all functions
python tools/dwarf_dump.py components/a/liba.a CRC5     # one function
```
