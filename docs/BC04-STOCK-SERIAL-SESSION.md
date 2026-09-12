# What to get from the stock BC04 over USB-C, before it is flashed

A working BC04 on the firmware it shipped with, on a bench, with a serial
console and an Ethernet cable. That combination has never existed in this
project before and will stop existing the moment the controller module is
swapped.

Ordered by what is **irreplaceable** first. Several items below cannot be
collected later at any price.

---

## 0. Before touching anything

- [ ] Confirm the board is the one baselined: SN `ALBC04ACA704F6DEE8`,
      MAC `ac:a7:04:f6:de:e8`, firmware `3.0.2 20260802`, THOR.
- [ ] Start capturing to a file *before* powering on. Everything below assumes
      the log starts at the bootloader banner, not at "I feel like logging
      now".
- [ ] 115200 baud. Either a plain serial terminal or `idf.py monitor`; a raw
      terminal is safer because it cannot reset the board.

**Do not** let anything write to flash. No `esptool write_flash`, no erase, no
`idf.py flash`. Reads only until section 5.

---

## 1. THE measurement — a linked W5500 through the core rail transient

This is the whole reason the board is on the bench. Everything else on this
page is worth less than this one item.

**Why it matters.** The previous BC04's W5500 stopped answering ~70 ms after
`vcore power on hashboard` and eventually failed short across the 3.3 V rail,
taking the TMP75, the EMC2302 and the I²C pull-ups with it. The boot order that
stands Ethernet in front of that transient is the *vendor's* — `network_init`
at `main.c:293`, `init_all_peripherals` at `main.c:320` in their published
source, and the THOR rewrite kept it. **Nobody has ever observed the failure on
factory firmware**, because by the time the dead board ran stock it was already
dead.

**Why the two logs so far do not count.** Both booted on WiFi with no cable.
The W5500 initialised but never linked, so it was idle when the rail came up.
The board that died was linked, addressed and passing traffic.

**Also known, and it sharpens this.** VCORE is enabled within ~100 ms of the
miner acquiring an IP address — 150 ms on one boot, 90 ms on another, whose
association times differed by 3.5 seconds. It tracks the network event, not a
timer. So with Ethernet as the network path, the W5500 will be linked,
addressed and seconds out of DHCP at the instant the rail steps up.

**Procedure**

- [ ] Ethernet plugged in and linked *before* power-on.
- [ ] If possible, make Ethernet the path that satisfies the boot: clear or
      wrong-out the WiFi credentials so it cannot fall back. Otherwise WiFi may
      win the race and leave the W5500 idle again.
- [ ] Cold power-cycle — pull the XT-30, not a soft reset. A soft reset does
      not re-run the power-on sequence.
- [ ] Capture continuously from the bootloader banner through
      `Enabling VCORE`, the ASIC scan, and at least five minutes of mining.

**What to look for, in order**

- [ ] `net_w5500: Ethernet ... initialized` and then a **link up** and an
      Ethernet IP — this is the state the earlier logs never reached.
- [ ] `Set VCORE to 4.65 V` / `Enabling VCORE`. Note the timestamp.
- [ ] The next 200 ms. Any `w5500.mac:` error, `send command timeout`,
      `issue RECV command failed`, or link loss. The old board produced the
      first error 70 ms after the rail came up.
- [ ] Whether the interface keeps its address while passing nothing — that was
      the old symptom, and it looks like a working network from the outside.
- [ ] Ping it over Ethernet afterwards, and load a page. The log alone will not
      tell you it stopped forwarding.

- [ ] **Repeat three times.** One clean boot proves less than three.

**Then the inverse**, which is the control:

- [ ] Boot with no cable, wait until mining is stable, *then* plug Ethernet in.
      If it links and stays healthy that way but not the other way round, the
      ordering is confirmed on a second unit.

**Record either outcome.** If it survives, the ordering theory weakens and the
dead board was faulty in a way this one is not — that is a real result and it
changes what `docs/RMA-BC04.md` can claim. If it wedges, the same fault is on a
second unit on unmodified firmware, which is the strongest statement this
project could make.

---

## 2. The controller module is the only real backup

Hammer's modules are locked — Secure Boot v2 with revoked keys, flash
encryption, JTAG disabled. That has two consequences people get wrong:

- A flash dump of the stock module is likely **ciphertext**, and cannot be
  restored onto a different module even if it reads cleanly.
- Therefore **the original module, physically intact and unflashed, is the
  restore path.** Not a file.

- [ ] Attempt `esptool read_flash 0x0 0x1000000 stock-full.bin` anyway and
      record what comes back. Even ciphertext establishes what was there.
- [ ] Read the partition table specifically: `esptool read_flash 0xd000 0xc00`.
      Our OTA container assumes that offset; confirm THOR agrees.
- [ ] `esptool summary` / eFuse read if it will answer — record whether Secure
      Boot, flash encryption and JTAG disable are actually set on **this**
      module, rather than assuming it matches the last one.
- [ ] **Label the original module and put it somewhere safe before the swap.**
      Write the serial on it. It is the only way back to a known-good stock
      BC04.

---

## 3. Things only the stock firmware can tell us

Once ours is on there, these answers are gone.

- [ ] **Full boot sequence at maximum verbosity.** Order of I²C init, display,
      network, VCORE, ASIC reset, frequency ramp. Our fork's ordering bugs were
      all "X before the thing X depends on"; this is the reference.
- [ ] **The I²C bus scan.** Which addresses answer: expect the regulator, the
      EMC2302 at 0x2e, the TMP75 at 0x48. Confirm nothing else is present that
      our firmware does not know about.
- [ ] **The voltage and frequency it actually uses.** It runs 4.65 V at
      760 MHz for ~5.75 TH/s at 89 W. Our ceiling is 480 (4.80 V) — *above*
      what the vendor now ships. Worth knowing whether 4.80 is still sane for
      this revision or whether our ceiling should come down.
- [ ] **The frequency ramp.** 50 MHz → 760 MHz in about 11 seconds. Note the
      step size and dwell; ours ramps differently.
- [ ] **The fan story.** `Fan0: 0 RPM` on all 81 baseline samples, `Fan1`
      around 3683. Determine whether a second fan is physically fitted, whether
      the header exists, and which EMC2302 channel drives which. Our
      `read_fan_rpm` takes `max(ch1, ch2)` on a BC04, which would hide a
      genuinely dead fan — this is the board that can settle it.
- [ ] **What the thermal PID does.** THOR has `thermal_pid`; ours has a step
      curve. Capture fan response against temperature over a range if the board
      will warm up enough.
- [ ] **Behaviour on the way down.** Watch a clean shutdown/restart and see
      whether it de-energises the hashboard first, and what it logs. Ours now
      reports a failed power-off; theirs may not.

---

## 4. THOR-specific unknowns worth resolving

Not blocking, but cheap while the console is attached.

- [ ] **Which HTTP endpoints actually exist.** The shipped web bundle calls
      `/api/claw/*` — LLM, WeChat login, Lua modules, file upload — and none of
      them answered when probed, nor did any endpoint from the older firmware.
      The serial log may show what is registered at startup.
- [ ] **The embedded `web.tar`.** THOR loads a 727040-byte tar rather than
      mounting a SPIFFS `www` partition. If the partition layout differs from
      ours, our www OTA path does not apply to this firmware at all.
- [ ] **Outbound connections.** `btc_mkt` fetches a price feed over HTTPS and
      failed three times overnight. Note anything else it calls out to; this is
      a mining device on someone's home network.
- [ ] **The API password.** The DC02's stock firmware prints `password is root`
      in plain text at every boot. Check whether THOR does the same, and what
      the default is on this unit.

---

## 5. Immediately before the module swap

- [ ] Final full-length log saved and backed up off the bench machine.
- [ ] Baseline numbers re-confirmed so the comparison after flashing is fair:
      **5746.8 GH/s average, 89.3 W, 51.5 °C board, 58.7 °C regulator, 0
      rejects, 0 hardware errors over 6.9 hours.**
- [ ] Original module labelled and stored.
- [ ] Photograph the board, both sides, before anything is unseated.

Then, and only then, swap the display module and continue with
[BC04-ACCEPTANCE.md](BC04-ACCEPTANCE.md).

---

## What we are *not* doing on this board yet

- Not flashing our firmware until section 1 has an answer and section 2 has a
  labelled module.
- Not writing to flash at all before then.
- Not probing its HTTP API from a script. The last attempt filled the owner's
  log with 404s and taught us nothing that serial will not say better.
