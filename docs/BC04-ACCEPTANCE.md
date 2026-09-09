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

## 2. The core-voltage ceiling — 2.0.28, never exercised

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
