# The BC04 warranty claim

A record, because this repository documents a fault in a product and would be
incomplete without saying what happened when the fault was reported.

It is written to be read by someone holding the opposite view. Everything here
that cuts against this project is in it, because the alternative is a page that
collapses the first time somebody reads our own documentation.

## What happened

A BC04 — label serial `ALBC04ACA7041FC978`, board V05, hardware v1.2.0 — was
bought new, supplied assembled in its case, and failed in August 2026.

It permanently lost its hashboard power domain. On the vendor's own firmware
3.0.0 20260718, with the board's **original** controller module refitted: no
chips detected, no input voltage at the regulator, no core voltage, no board
temperature, no fan RPM. The controller side is fine — it boots, drives the
display, joins WiFi and serves its interface. 12.3 V is present and correct at
the XT-30.

The physical findings, measured:

- With the case screws fitted, a mounting screw reached **109 °C**.
- Heat concentrated behind the controller module, immediately around the W5500.
- The controller module's 3.3 V regulator overheated within seconds.
- A short in the 3.3 V path.
- With the screws removed the board powers up normally, and the hashboard
  domain still reads zero.

A warranty claim was filed with an evidence bundle: kernel logs, a bus scan, a
pin survey, telemetry from while the board was healthy, and a capture of the
board on the vendor's own firmware reporting the I²C devices absent.

**That bundle is not published here.** It was prepared for the vendor and
contains correspondence and contact details that are not theirs to have
published. Filenames from it are named below so the claims can be traced if it
is ever shared, but a reader of this page cannot open them, and nothing on this
page should be believed on the strength of a citation nobody can check. What
can be checked is in this repository: the logs quoted in
[HARDWARE-SAFETY.md](HARDWARE-SAFETY.md) and [KNOWN-ISSUES.md](KNOWN-ISSUES.md),
and the commit history.

**The claim was refused. The stated ground was that third-party firmware had
been run on the board.**

## The refusal is on fair terms, and this project says so

This firmware is unofficial. The README tells every reader, before the download
link, that flashing it may void whatever warranty they have. It would be
incoherent to print that warning for other people and then call it unjust when
it was applied here.

A manufacturer is entitled to decline warranty on a device that has run
software they did not write. That is a normal term, it was known in advance,
and nothing here asks anyone to think otherwise.

It is also a **contractual** ground, not a technical one. No amount of
operating data answers it, and this page does not pretend otherwise. Warranty
and causation are different questions, and only the second is discussed below.

## What the record establishes

**The same failure class has occurred on a board that never ran this
firmware.** A second BC04, `ALBC04ACA704F6DEE8`, arrived 2026-09-11 and was run
on stock THOR 3.0.2 — nothing of this project's was ever flashed to it, no
settings were changed, nothing was opened. It baselined healthy for 6.9 hours
at 5,747 GH/s with 7,471 shares and zero hardware errors, then lost every I²C
device: `TPS546 not found on I2C bus`, `Failed to write TMP75 configuration
register`, transaction timeouts. A vendor firmware update applied afterwards
did not restore it.

**That cuts both ways, and the cut matters.** That board's W5500 still
initialises, links and takes an address after the fault. Its failure is
therefore **not** the W5500-short mechanism. It shows that a BC04 can lose its
I²C domain on the vendor's own firmware. It does not show that the boot order
killed the first board.

**The W5500 behaviour on the failed unit, on this firmware.** Linked and
addressed, then the first SPI timeout 70 to 130 ms after the core rail stepped.
Reproducible across at least five cold boots, never recovering within a
session, recovering on a full power cycle.

**The boot order is the vendor's, on the evidence of a measurement.** Stock
THOR 3.0.2, measured on a different BC04, initialises `net_w5500` at t ≈ 2.7 s
and enables VCORE at t ≈ 10.9 s — the Ethernet controller is up before the core
rail steps. The vendor's published source shows the same order at `main.c:293`
and `main.c:320`, but that is their pre-production build, which their own
README calls separate from and unrelated to THOR; the measurement is the
citation that carries, not the line numbers.

## What the record does not establish

**There is no reproduction.** The capture of this board on vendor firmware
shows an already-dead board being correctly reported. It was taken after the
failure. A reproduction would be a healthy board failing under stock firmware,
and nothing in the record is that.

**The ordering was never observed killing a linked controller.** Both
stock-THOR boots were on WiFi with no cable, so that W5500 was initialised but
never linked — idle, not the state the failed board's was in. The one
measurement that would close this has never been taken.

**This firmware put the part through a larger transient than the vendor's
does.** The fork commanded **4.80 V** at every power-on while configured at
4.60 V. Stock THOR sets 4.65 V directly. Whatever the core-rail step did to
that controller, ours was the bigger one.

**And this firmware kept getting the ordering wrong after it was first fixed.**
An external review on 2026-09-28 found three further paths that still stood the
W5500 in front of the core rail, two of them in our own code, and
`01-healthy-boot.log` shows one of them running **on this very board** — the
self test powering the rail at 4.00 V with the W5500 dead 320 ms later. The
fork also panicked eight times in ten boots from a defect in its own fan
driver, each panic after the hashboard had been powered: real extra rail cycles
that this firmware caused.

**Operating within limits does not answer the question.** Latch-up is triggered
by a supply step, not by a steady-state operating point, so "we ran at or below
the vendor's numbers" does not address the mechanism. It is also not quite true
as stated: the board spent most of its life **at** the vendor's shipped
750 MHz / 4.70 V over-frequency profile rather than below it, and a tuning sweep
applied 775 MHz for about four minutes, above any vendor profile. What is true
and narrower: thermals stayed well inside the firmware's own 71 °C trip, peaking
at 65 °C chip and 73 °C regulator; the unit never reached its 115 W rating,
peaking at 106 W; and there were zero hardware errors at last reading. That
answers an allegation of abuse. It does not bear on causation.

**No recorded regulator fault is not evidence that none occurred.** Nothing was
looking: `VOUT_OV_FAULT_LIMIT` sits inside an `#if 0`, `VCORE_check_fault()` is
commented out, and `STATUS_WORD` has never been read on any board here.

**Every failure capture was taken with a replacement controller module
fitted.** The module-versus-base-board check that [BOARDS.md](BOARDS.md) prescribes
was never performed on this unit.

**The pin survey's "shorted I²C bus" reading was withdrawn** in the evidence
bundle itself, and should not be cited without that.

## Four explanations fit, not one

This project's own record contains four, and the physical evidence does not
choose between them:

1. **W5500 latch-up** from the core-rail transient, ending as a short across
   3.3 V. Consistent with the recover-only-on-power-cycle signature and with
   the end state.
2. **The XT-30 disconnection.** [HARDWARE-SAFETY.md](HARDWARE-SAFETY.md) records a board that
   "failed permanently with an electrical smell at the moment its XT-30 was
   disconnected".
3. **Transport or mechanical damage**, which the internal notes record as
   matching the timeline.
4. **Board flex under the mounting screws.** The 109 °C screw sits beside the
   W5500, and the correction note records PE7 — that mounting point — showing
   no short.

The first is the one this project has pursued hardest. That is a reason to be
careful with it, not a reason to prefer it.

## Why this is on the repository

Two reasons, neither of them grievance.

[HARDWARE-SAFETY.md](HARDWARE-SAFETY.md) describes a power-on order that runs the W5500
through the core-rail transient. A reader weighing that is entitled to know
that the board it was observed on is dead, that the claim was refused, and on
what grounds — otherwise the finding reads as more settled than it is.

And anyone running this firmware should understand their position before they
flash: if the hardware fails afterwards, the warranty conversation may end the
same way. That is the honest version of the warning already in the README.

## Status, and a limitation that is now permanent

The board was not returned. A replacement was purchased.

The deciding measurement — whether a **linked** W5500 survives the first
hashboard power-up on the vendor's own firmware — has never been taken, and at
this point probably never will be. Two boards have come and gone without it:
the second died before the test could be run, and the third was soaked on stock
and then flashed. That is a permanent limitation of this record rather than a
pending result, and this page does not promise to get stronger later.

What has since been established on working hardware is narrower, and is a
statement about this firmware rather than about the vendor's: holding the W5500
in hardware reset across the rail step works, and the part comes back alive.
That closes the hazard going forward. It says nothing about what killed this
board.

Nothing here is an allegation of bad faith. A refusal on stated warranty terms
is not evidence of anything except the terms.

This page is about a warranty claim and nothing else. Correspondence with the
vendor about source availability is a separate matter and is not part of it.
