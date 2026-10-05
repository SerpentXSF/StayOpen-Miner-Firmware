# Hammer firmware: what is published, and where

A record of what Hammer Miner has actually released, gathered from primary
sources so the claims in this repository's documentation can be checked rather
than taken on trust.

**Collected:** 30 August 2026, via the GitHub REST API.
**Method:** every figure below came from `api.github.com`, not from a web page,
a screenshot, or anyone's account of it. The commands are given so the same
numbers can be produced independently. Where a document is quoted it is quoted
exactly, and the source is linked.

Nothing here is an inference about intent. Where something is an opinion or a
judgement it is marked as one.

---

## Summary

Hammer's code is spread across two GitHub organisations. One holds compiled
binaries and no source. The other holds full firmware source for three models,
was created the day that source appeared, carries no licence file, and is not
referenced from anywhere in the first.

---

## 1. The `HammerMiner` organisation

<https://github.com/HammerMiner>

```bash
gh api users/HammerMiner
```

| | |
|---|---|
| Type | Organization |
| Display name | Hammer Miner |
| Website | www.hammerminer.com |
| Location | United States of America |
| Created | 2026-04-08 |
| Public repositories | 17 |

### 1.1 What the repositories contain

```bash
gh api "orgs/HammerMiner/repos?per_page=100" --paginate
```

| Repository | Size (KB) | Last push | Description |
|---|---|---|---|
| BC01-APP | 0 | 2026-06-25 | 1-Asic BTC Miner |
| BC01-WWW | 0 | 2026-06-25 | Miner UI |
| BC04 | 4 | 2026-07-12 | 4-BM1370: BTC Miner; Thor OS |
| BC04-APP | 0 | 2026-08-27 | 4-Asic BTC Miner |
| BC04-WWW | 0 | 2026-06-25 | Miner UI |
| BC08 | 1 | 2026-08-21 | BC08 solo home miner |
| DC02-APP | 1 | 2026-06-27 | 2-Asic LTC Miner |
| DC02-V1-APP | 3 | 2026-04-15 | 2-Asic LTC Miner |
| DC02-V1-WWW | 4 | 2026-04-15 | Miner UI |
| DC02-WWW | 1 | 2026-06-27 | Miner UI |
| DC04-APP | 0 | 2026-06-29 | 4-Asic LTC Miner |
| DC04-WWW | 0 | 2026-06-29 | Miner UI |
| DC06-APP | 0 | 2026-06-27 | 6-Asic LTC Miner |
| DC06-WWW | 0 | 2026-06-27 | Miner UI |
| P2 | 2 | 2026-08-19 | |
| X1 | 9 | 2026-08-28 | test |
| hammer-claw-skills-lab | 2061 | 2026-08-04 | skills-lab |

Sixteen of the seventeen are between 0 and 9 KB.

### 1.2 The BC01 repositories specifically

```bash
gh api repos/HammerMiner/BC01-APP/contents
gh api repos/HammerMiner/BC01-APP/commits
```

| | `BC01-APP` | `BC01-WWW` |
|---|---|---|
| Files in tree | `README.md` only | `README.md` only |
| README size | 28 bytes | 20 bytes |
| Commits | 1 | 1 |
| Commit message | "Initial commit" | "Initial commit" |

`BC01-APP/README.md`, in full:

```
# BC01-APP
1-Asic BTC Miner
```

There is no firmware source, no build script and no configuration in either
repository.

### 1.3 What is actually distributed

The payloads are release assets, which do not count toward repository size.

```bash
gh api repos/HammerMiner/BC01-APP/releases
```

| Repository | Releases | Latest tag | Asset |
|---|---|---|---|
| BC01-APP | 2 | V2.0.3-20260625 | `bc01-miner-2.0.3-20260625-update.bin` |
| BC01-WWW | 2 | V1.4.0-20260625 | `bc01-www-1.4.0-20260625-update.bin` |
| BC04-APP | 10 | V3.0.4-20260826 | `bc04-3.0.4-20260826-update.bin` |
| BC04 | 1 | v1.0.1 | `Thor-BC04.bin` |
| DC02-APP | 3 | V2.0.2-20260627 | `dc02-miner-2.0.2-20260627-update.bin` |
| DC02-WWW | 3 | V1.3.8-20260627 | `dc02-www-1.3.8-20260627-update.bin` |
| DC04-APP | 1 | V2.0.2-20260629 | `dc04-miner-2.0.2-20260629-update.bin` |
| DC06-APP | 2 | V2.0.2-20260625 | `dc06-miner-2.0.2-20260625-update.bin` |
| BC08 | 2 | v1.0.2-20260820 | `bc08-1.0.2-20260820-update.bin` |
| X1 | 3 | v1.0.4-20260827 | `thorx1-1.0.4-20260827-update.bin` |
| P2 | 2 | v1.0.1-20260818 | `thorp2-1.0.1-20260818-update.bin` |

Every artifact is a compiled `.bin`. Binary releases were still being published
in late August 2026 — `BC04-APP` most recently on 2026-08-26.

### 1.4 The damage warning

`HammerMiner/DC02-APP/README.md`, quoted exactly:

> Firmware Upgrade Compatibility Notice
> -2.x.x devices: Flash this firmware (DC02-APP).
> 1.x.x devices: Please download the corresponding firmware from [GitHub Releases](https://github.com/HammerMiner/DC02-V1-APP).
> (Warning: Upgrading 1.x.x firmware with this firmware will cause device damage)

No recovery procedure accompanies it.

---

## 2. The `baichuan-org` organisation

<https://github.com/baichuan-org>

```bash
gh api users/baichuan-org
```

| | |
|---|---|
| Type | Organization |
| Display name | *(none)* |
| Website | *(none)* |
| Description / bio | *(none)* |
| Location | *(none)* |
| Created | **2026-08-21T03:29:59Z** |
| Public repositories | 3 |

### 2.1 What is there

| Repository | Size (KB) | Last push | Description |
|---|---|---|---|
| [BC01](https://github.com/baichuan-org/BC01) | 68,126 | 2026-08-21 | BC01 lab version from Chengdu Baichuan |
| [BC04](https://github.com/baichuan-org/BC04) | 67,618 | 2026-08-21 | |
| [DC02](https://github.com/baichuan-org/DC02) | 69,872 | 2026-08-21 | DC02 lab version |

`baichuan-org/BC01` top level:

```
CMakeLists.txt   README.md      components/    config.cvs
dependencies.lock  idf_component.yml  main/    merge_bin.sh
package-lock.json  package.json   partitions.csv  sdkconfig
setup.py
```

`main/` holds 27 entries including `main.c`, `device.c`, `global_state.h`,
`http_server/` and `displays/`.

**This is genuine, complete, buildable firmware source** — a standard ESP-IDF
project. Any claim that Hammer released no source is wrong.

### 2.2 No licence file

```bash
gh api repos/baichuan-org/BC01/license   # 404
```

None of the three repositories contains a `LICENSE` or `COPYING` file, and
GitHub detects no licence for any of them. The release states its purpose is
satisfying GPL obligations; GPL-3.0 §4 requires the licence text accompany the
source.

For contrast, this repository carries `LICENSE` (GPL-3.0, detected by GitHub)
along with [PROVENANCE.md](PROVENANCE.md) and [CREDITS.md](CREDITS.md).

### 2.3 The README, quoted in full

<https://github.com/baichuan-org/BC01/blob/main/README.md>

> This repository contains the firmware source code for the pre-production
> product that was released to the market in limited quantities.
>
> Why we are open-sourcing this code: Due to time constraints during the early
> development phase, the firmware was outsourced to Chengdu Baichuan for
> development. At that time, our team was unaware that the deliverable
> incorporated code derived from ESP Miner. Following an internal compliance
> audit, we have determined that the GNU General Public License (GPL)
> obligations apply. In full compliance with the GPL, we are now releasing the
> corresponding source code.
>
> Important distinction: The Hammer R&D team has since embarked on a clean-room
> effort to develop its own firmware operating system from the ground up, with
> strict independence from ESP Miner from day one. The code in this repository
> represents the laboratory version of the pre-production product only. It is
> entirely separate from and unrelated to our subsequent self-developed systems,
> including but not limited to THOR OS, NORN OS, and GLOD OS.
>
> Maintenance status: The Hammer team will not be able to provide ongoing
> maintenance, support, or continued development for the code released in this
> repository. This release is provided as-is to satisfy our GPL compliance
> obligations.

Three things this establishes on the record:

1. **An ESP-Miner derivation is acknowledged**, and GPL obligations accepted.
2. **It is scoped** to the outsourced pre-production build. THOR OS, NORN OS and
   GLOD OS are asserted to be clean-room and unrelated. This document takes no
   position on that assertion, which is not independently verifiable from
   published material.
3. **The released code is unmaintained by their own statement.**

The voice is Hammer's, not Baichuan's: it is Hammer who *"outsourced to Chengdu
Baichuan"* and Hammer whose *"R&D team has since embarked"*. Baichuan is the
subject described, not the author.

---

## 3. The two organisations are operated by the same people

Commit metadata is public. Identities below are GitHub account names; the
underlying addresses are visible through the API and are not reproduced here.

```bash
gh api "repos/baichuan-org/BC01/commits"
gh api "repos/HammerMiner/hammer-claw-skills-lab/commits?per_page=100"
```

| Account | In `baichuan-org` | In `HammerMiner` |
|---|---|---|
| `EdwinSong` | **both** commits to `BC01`, including "Update README with firmware and compliance details" (2026-08-21) | **8 of 52** commits to `hammer-claw-skills-lab` (2026-08-04) |
| `xieliyi2026` | — | `BC01-APP`, `BC01-WWW`, `BC04-APP`, `DC02-APP` |
| `Hammer-Miner` | — | `hammer-claw-skills-lab` |
| `liuhq123321` | — | `BC08`, `X1`, `hammer-claw-skills-lab` |

Commits under `HammerMiner` are authored from the corporate domain
`gullpower.com`.

The account that wrote Hammer's compliance statement holds commit access inside
Hammer's own organisation. The source release is Hammer's own work, not a third
party's.

---

## 4. Nothing links them publicly

Every README in the `HammerMiner` organisation was checked for any mention of
`baichuan`, `GPL`, `source`, or `licence`:

```
BC01-APP     no mention        BC08         no mention
BC01-WWW     no mention        X1           no mention
BC04-APP     no mention        P2           no mention
BC04         no mention        DC02-V1-APP  no mention
DC02-APP     no mention
```

Neither organisation publishes its members. `baichuan-org` has no display name,
website or description.

**The practical effect:** an owner who goes to Hammer's GitHub looking for the
source to their miner finds compiled binaries and a 28-byte README, with nothing
indicating that source exists or where to find it. Whether that is deliberate is
not something this document can establish. That it is the outcome is a matter of
record.

---

## 5. What is not claimed here

Kept separate so it is clear where the evidence stops.

- **No position on whether THOR/NORN/GLOD OS derive from ESP-Miner.** Those are
  not published, so nobody outside Hammer can say. Their statement that these
  are clean-room is recorded above and is neither corroborated nor contradicted
  by anything available.
- **No claim that Hammer released nothing.** They released full source for three
  models. The findings concern licensing and discoverability, not existence.
- **No claim about intent.** Only about what is published, and where.
- **No claim about the shipping firmware's provenance**, beyond noting the
  acknowledged derivation applies to the pre-production build.
- **No claim that the vendor's silence means anything.** The questions in
  section 6a went unanswered. A company is not obliged to answer a customer's
  email, and an unanswered question is not an admission.

---

## 6. Reproducing this

```bash
gh api users/HammerMiner
gh api "orgs/HammerMiner/repos?per_page=100" --paginate
gh api repos/HammerMiner/BC01-APP/contents
gh api repos/HammerMiner/BC01-APP/commits
gh api repos/HammerMiner/BC01-APP/releases
gh api repos/HammerMiner/DC02-APP/readme

gh api users/baichuan-org
gh api "users/baichuan-org/repos?per_page=100"
gh api repos/baichuan-org/BC01/contents
gh api repos/baichuan-org/BC01/readme
gh api repos/baichuan-org/BC01/license      # 404
gh api repos/baichuan-org/BC01/commits
```

Figures were accurate on 30 August 2026. Repositories change; re-run before
relying on any number here.

## 5a. The vendor's own site does not lead to the source

<https://www.hammerminer.com/#/service#download>, read 30 August 2026.

The firmware download page offers "APP FIRMWARE" and "WEB FIRMWARE" for BC01,
BC04, DC02 and DC06. Every download control links to a GitHub releases page in
the `HammerMiner` organisation:

```
BC01 APP  -> github.com/HammerMiner/BC01-APP/releases
BC01 WEB  -> github.com/HammerMiner/BC01-WWW/releases
BC04 APP  -> github.com/HammerMiner/BC04-APP/releases
DC02 APP  -> github.com/HammerMiner/DC02-APP/releases
DC02 WEB  -> github.com/HammerMiner/DC02-WWW/releases
DC06 APP  -> github.com/HammerMiner/DC06-APP/releases
DC06 WEB  -> github.com/HammerMiner/DC06-WWW/releases
```

Those are the repositories shown in section 1 to contain no source — a README
and compiled `.bin` release assets.

A further link, "Browse All Firmware on GitHub", points to
`github.com/HammerMiner/` beneath this description:

> Find firmware releases, source code, and documentation for all Hammer Miner
> devices.

No source code is published in that organisation. The source is in
`baichuan-org`, which is not linked or named anywhere on the page.

**The consequence is concrete.** An owner following the vendor's own
instructions to find the source for their device is directed to compiled
binaries and told that is where the source code is. The corresponding source
exists, but nothing in the supply chain the owner is given leads to it.

Two other statements from the same page are worth recording, since they bear on
claims made elsewhere about which devices are current:

> THOR OS [...] ships natively on every new Hammer Miner — THOR X1, THOR P2 and
> BC08

> Existing Hammer Miner models BC01 and BC04 will receive dedicated THOR OS
> upgrade packages, keeping already-deployed hardware on the same platform.

The BC04 entry lists "V3.0.2 | 2026-08-02 — Upgrade to new THOR OS Firmware".
BC01 is therefore described by the vendor as scheduled to receive THOR OS, not
as discontinued.

Contact address published on the same page: `info@hammerminer.com`, described as
the "Official Support Channel".

## 6. Timeline

The vendor's release tags embed the firmware build date, which gives a dated
record independent of when anything was pushed to GitHub.

| Date | Event | Source |
|---|---|---|
| 2025-12-25 | Firmware `V1.0.0` built | tag `V1.0.0-20251225`, DC02-V1-APP |
| 2026-01-27 | `V2.0.0`, "The first officially released version" | tag + release notes, DC02-APP |
| 2026-04-08 | `HammerMiner` organisation created | org metadata |
| 2026-04-10 | First binaries published to GitHub | earliest release publication |
| 2026-05-06 | Earliest BC01 firmware build in evidence | tag `V2.0.1-20260506` |
| 2026-06-25 | `BC01-APP` repository created; BC01 binaries published | repo + release metadata |
| **2026-08-21** | **`baichuan-org` created; all three source repositories published** | org + repo metadata |
| 2026-08-30 | Questions put to `info@hammerminer.com`; reply window to 13 September | section 6a |
| 2026-09-13 | Reply window closed with no response | section 6a |

The earliest firmware build date the vendor's own tags record is **25 December
2025**. Corresponding source appeared **21 August 2026**, roughly eight months
later. For the BC01 specifically, the earliest published firmware build is dated
6 May 2026, about three and a half months before the source.

**What this does and does not establish.** These are publication and build dates
taken from vendor-controlled metadata. They do not establish when devices were
first sold, which is not visible from GitHub. GPL-3.0 §6 requires the
Corresponding Source to accompany the object code when it is conveyed, or a
written offer valid for three years to be given in its place. Whether that was
satisfied depends on when units were conveyed and what accompanied them —
questions only the vendor and its customers can answer. The dates above
establish when source became publicly available, and no earlier public source
release has been found.

## 6a. The vendor was asked, and has not replied

Nothing above was published without first putting it to Hammer Miner.

On **30 August 2026** an email was sent to `info@hammerminer.com`, the contact
address published on their own site, under the subject *"GPL-3.0 corresponding
source and Installation Information — request for confirmation and comment"*.
It said plainly that it was being sent before publishing further, invited
correction, and offered to publish any reply in full.

It asked five things. Quoting the substance:

**1. Are the source repositories yours?** "On 21 August 2026, three repositories
appeared at `github.com/baichuan-org` containing full firmware source for BC01,
BC04 and DC02. Each carries a README stating that the firmware was outsourced to
Chengdu Baichuan, that it incorporated code derived from ESP-Miner, and that GPL
obligations were determined to apply. That organisation has no display name,
website or description, and nothing on hammerminer.com or under
`github.com/HammerMiner` refers to it. Before I attribute those statements to
Hammer Miner, I would like you to confirm: is `github.com/baichuan-org` your
official GPL source release? If it is not, I will correct my published findings
accordingly."

**2. Reaching the source.** The download page links every firmware download to
the `HammerMiner` organisation and describes it as containing "firmware
releases, source code, and documentation", while that organisation contained no
source — "an owner following your instructions to find the source is directed to
binaries. A link from your download page would resolve this immediately."

**3. Licence text.** None of the three source repositories contains a LICENSE or
COPYING file; GPL-3.0 §4 requires the licence accompany the source.

**4. Timing.** The earliest firmware build in the vendor's own tags is dated 25
December 2025 and the source was published 21 August 2026: "For units sold
before August 2026, what was provided at the time of sale?"

**5. Installation Information, §6.** A retail BC01 reports Secure Boot enforced,
both spare key digest slots revoked and JTAG permanently disabled, so "an owner
can obtain the source, modify it and build it, but cannot run the result on
hardware they own." The email explicitly conceded that §6 does not oblige the
vendor to support or maintain modified firmware, and said that either signing
community builds on request or publishing the Installation Information "would
resolve this and I would document it as resolved."

It also stated this project's own position without being asked: that this
firmware is maintained under GPL-3.0 with full corresponding source and
attribution, that it involves **no circumvention** of the vendor's Secure Boot
because the ESP32-S3 is a socketed module and a new one is fitted, and that its
documentation tells owners to check `get_security_info` and stop if Secure Boot
is enforced. "I am not distributing a way past your locks and have no interest
in doing so."

A reply window of fourteen days was allowed, to **13 September 2026**.

**No reply has been received.** As of the date on this section, more than a
month after the email and three weeks after the window closed, Hammer Miner has
not responded.

**What that does and does not mean.** It means the questions above went
unanswered and the findings in this document stand uncorrected — not that they
are confirmed. Silence is not agreement. A company is under no obligation to
answer an email from a customer, and nothing here should be read as suggesting
otherwise. Question 1 in particular remains genuinely open: whether
`baichuan-org` is Hammer's official source release is still unconfirmed by
anyone at Hammer, and this document continues to treat it as an attribution
that has not been acknowledged rather than one that has been denied.

The offer stands. If a reply arrives it will be published here in full, and
anything shown to be inaccurate will be corrected rather than defended.

The sending address is omitted here deliberately; this repository does not
publish personal contact details, including its maintainer's.

## 7. Preservation

Published repositories can be withdrawn. What is recorded here does not depend
on them staying up.

Each source repository was cloned in full on 30 August 2026. A git commit id is
a cryptographic commitment to the entire tree, so a copy can be proven identical
to what was published by checking its HEAD against these:

| repository | HEAD commit |
|---|---|
| `baichuan-org/BC01` | `8dab8f4b1e7b81eea0f3063b09619677a1252dca` |
| `baichuan-org/BC04` | `059321423fc2608d1acb229e64f466233e24dbff` |
| `baichuan-org/DC02` | `55d10fd4ca07f16efbe195cd6805a69f790da46e` |

The BC01 id is the same commit this firmware's `main/device.c` already cites for
the imported USB-PD sequence, so that attribution is checkable against the
archive independently.

Alongside the clones, the raw API responses and the READMEs quoted above were
captured verbatim, each recorded with its SHA-256 in a manifest. Those files are
held outside this repository — they are evidence, not part of the firmware, and
none of it is anything the licence prevents anyone from redistributing.

Screenshots were deliberately not relied on. An image cannot be verified against
anything; a commit id and a digest can.

## Related

- [PROVENANCE.md](PROVENANCE.md) — how this firmware descends from its upstreams
- [FINDINGS.md](FINDINGS.md) — defects found in the shipped firmware
- [SECURITY.md](SECURITY.md) — security findings and mitigations
- [CREDITS.md](CREDITS.md) — the open source work this builds on
