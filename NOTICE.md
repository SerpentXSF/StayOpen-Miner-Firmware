# Copyright and attribution

This firmware is a derivative work of **ESP-Miner**, released under the
GNU General Public License version 3. The full license text is in
[LICENSE](LICENSE). Every copy or modified version of this firmware must
be distributed under the same terms, with complete corresponding source.

## Upstream projects

| Project | Author | License | Relationship |
|---|---|---|---|
| [ESP-Miner](https://github.com/bitaxeorg/ESP-Miner) | Bitaxe / Skot and contributors | GPL-3.0 | Primary upstream. Task model, Stratum client, mining pipeline, ASIC drivers, NVS config layer, self-test, and the web UI all derive from it. |
| [NerdQAxe+](https://github.com/shufps/ESP-Miner-NerdQAxePlus) | shufps and contributors | GPL-3.0 | Origin of the InfluxDB telemetry component. |
| [LVGL](https://github.com/lvgl/lvgl) | LVGL Kft. | MIT | Display library, fetched as an unmodified dependency. |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Espressif Systems | Apache-2.0 | Build system, RTOS, and drivers. |
| BC04 firmware | Chengdu Baichuan, for Hammer | GPL-3.0 | The vendor release this repository is built from. |
| [BC01 firmware](https://github.com/baichuan-org/BC01) | Chengdu Baichuan, for Hammer | GPL-3.0 (derived; no LICENSE supplied) | Source of the USB-PD stage this tree needs for the BC01. `components/bc_hal/HUSB238A.c` and `.h` are imported verbatim; the PD bring-up in `main/device.c` and the BC01 GPIO and voltage configuration come from it. Imported at commit `8dab8f4`. |
| SerpentX Dual Pool Mining (BitAxe / NerdAxe) | SerpentX | GPL-3.0 | Origin of the dual-pool work. `components/dual_pool/` is imported from it; the pool B session and the create_jobs / asic_result hooks are ported to this tree's task layout. |

## Fonts compiled into the firmware

Converted to LVGL C arrays under `main/displays/images/`; the originals are in
`main/displays/images/fonts_originals/`. They ship inside the binary, so their
licences travel with it.

| Font | Licence | Where |
|---|---|---|
| VT323 | SIL Open Font License 1.1 | `ui_font_*` display faces |
| Open Sans Bold | Apache License 2.0 | `ui_font_OpenSansBold*` |
| DigitalNumbers Regular | **not established** | `ui_font_DigitalNumbers*` |

DigitalNumbers arrived without a licence file and one has not been traced. It
is recorded here as unresolved rather than assumed permissive: that is the
honest state, and it is the kind of gap this repository exists to point out in
other people's releases.

## Web interface dependencies

Bundled into `www.bin` and therefore shipped in the firmware. All MIT, which
is compatible with GPL-3.0 and requires the notice to travel with the binary.

| Package | Licence |
|---|---|
| vue | MIT |
| ant-design-vue | MIT |
| axios | MIT |
| pinia | MIT |
| vue-router | MIT |
| vue-i18n | MIT |
| uplot | MIT |
| js-cookie | MIT |
| js/sha256.min.js | MIT |

The build stripped these notices out of the bundle until 2026-09-28:
`vite.config.ts` set `comments: false`, which removes `@license` banners along
with everything else. It is `comments: 'some'` now, so the banners ship.
Removing them was a licence violation, and a poor one for a project that
exists because somebody shipped GPL code without its source.

## Why this file exists

The vendor's release at `github.com/baichuan-org/BC04` shipped **no license
file and no attribution of any kind**. GPL-3.0 section 4 requires that
anyone conveying the work also convey the license text and keep all
copyright notices intact. Publishing the source alone does not satisfy
that; the notices have to travel with it.

This file, [LICENSE](LICENSE), and [docs/PROVENANCE.md](docs/PROVENANCE.md)
restore what should have accompanied the original release.

## Attribution is not a formality here

ESP-Miner is the work of independent developers who chose the GPL
specifically so that hardware vendors could not take their work private. They are
named individually in [docs/CREDITS.md](docs/CREDITS.md); this file satisfies
the licence, that one says thank you.
A commercial product was built on it, sold, and shipped closed-source.
The vendor's own README describes this as an oversight discovered during
a compliance audit.

Whatever the intent, the practical effect was that the people whose work
made this product possible were not credited to its buyers. Naming them
is the point of the license.

## Imported from the BC01 release

`components/bc_hal/HUSB238A.c` and `include/HUSB238A.h` are **verbatim
copies** from `baichuan-org/BC01` at commit `8dab8f4`, kept byte-identical
so they can be diffed against the original. The USB Power Delivery
bring-up in `main/device.c` is imported from the same source with only the
changes needed to fit this tree's structure, and is marked as such in the
code.

That release carries **no LICENSE file**. It is nonetheless a derivative of
ESP-Miner — it is the same codebase as the BC04 release, sharing its file
layout and its `health_maintennance.c` misspelling — so GPL-3.0 applies to
it by inheritance regardless of what the repository does or does not state.
This repository treats it accordingly and licenses the result under
GPL-3.0.

Their copyright in these files remains theirs. Nothing here claims
authorship of imported work.

## Scope of this repository's own changes

Changes made here — security fixes, the BC01 port, the reimplemented ASIC
abstraction layer, tooling, and documentation — are likewise GPL-3.0, and
are offered back under the same terms.
