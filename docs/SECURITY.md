# Security assessment

Findings against the BC04 source as released, and against a BC01 running
stock firmware 2.0.1. Each finding states how it was confirmed.

**If you run a Hammer miner today, read [Mitigations](#mitigations) first.**
The issues below are in shipping firmware, not only in this source tree.

---

## Summary

| # | Severity | Issue | Status here |
|---|---|---|---|
| [1](#1-no-authentication-on-any-endpoint) | Critical | No authentication on any HTTP endpoint | Fixed |
| [2](#2-unauthenticated-ota-with-obfuscation-standing-in-for-integrity) | High / Critical | Unauthenticated OTA; impact depends on whether Secure Boot eFuses were burned | Mitigated |
| [3](#3-credentials-shipped-in-the-factory-image) | High | WiFi password, API token, third-party payout address in factory NVS | Fixed |
| [4](#4-uninitialised-ota-handle-used-on-error-paths) | Medium | Uninitialised `esp_ota_handle_t` passed to `esp_ota_abort()` | Fixed |
| [5](#5-telemetry-enabled-by-default) | Low | InfluxDB reporting on by default to a hardcoded host | Fixed |

---

## 1. No authentication on any endpoint

The web UI ships a login page. [`Login.vue`](../main/http_server/axe-os/src/pages/Login.vue)
collects a username and password, and
[`api/index.ts`](../main/http_server/axe-os/src/api/index.ts) POSTs them to
`/api/system/login`.

**That route does not exist in the firmware.** The registered URI table in
`http_server.c` contains no login handler, no session issuance, and no
credential check. The login screen is decorative.

Every endpoint is instead gated by `is_network_allowed()`, which resolves
to:

```c
if (ip_in_private_range(origin_ip_addr) == ESP_OK &&
    ip_in_private_range(request_ip_addr) == ESP_OK) {
    return ESP_OK;
}
```

This asks "is the caller on a private network", not "is the caller
authorised". It is a DNS-rebinding mitigation being used as an access
control. Any host on the same LAN — a guest device, a compromised IoT
appliance, a browser on a colleague's laptop — has full administrative
access.

Confirmed live against a stock BC01 at 2.0.1 with no credentials:

```console
$ curl -s http://<miner>/api/system/info
{ "hostname": "HammerBC01", "ASICModel": "BM1370", "stratumURL": ...,
  "stratumUser": "bc1q...", "ssid": "...", ... }
```

The same lack of a check applies to `PATCH /api/system` (repoint the pool),
`POST /api/system/restart`, `POST /api/system/OTA`, and the rest.

An attacker who redirects `stratumURL` and `stratumUser` steals the
device's entire hash output, and the change survives reboot because it is
written to NVS.

### Fix

A shared-secret check is enforced in the firmware for every state-changing
and information-disclosing endpoint, with the network-range test retained
as defence in depth rather than as the sole control. See
[`docs/AUTH.md`](AUTH.md).

---

## 2. Unauthenticated OTA, with obfuscation standing in for integrity

`POST /api/system/OTA` accepts an update image, deobfuscates it, verifies a
SHA-256 that travels inside that same image, writes it to the inactive OTA
slot, and calls `esp_ota_set_boot_partition()`.

**The endpoint requires no authentication.** Per finding 1, any host on the
LAN can start an update.

### What the obfuscation does not do

The images are not encrypted. The scheme is a repeating 16-byte XOR pad
recoverable from any published firmware image without the key -- full
analysis in
[OTA-FORMAT.md](OTA-FORMAT.md#the-obfuscation-is-not-encryption). The
SHA-256 in the header is computed over the plaintext and obfuscated with
that same pad, so it authenticates nothing: an attacker recomputes it over
their own payload.

Verified: `tools/ota_tool.py` unpacks the vendor's own
`bc01-miner-2.0.3-20260625-update.bin` and repacks it byte for byte, using
a pad recovered from ciphertext alone with no key material.

The key is compiled into the application image
(`_binary_flash_encryption_key_bin_start`), so it is identical across units
of a model and extractable from any one of them.

### What does provide integrity

Secure Boot v2. The vendor's `sdkconfig` enables it
(`CONFIG_SECURE_BOOT_V2_ENABLED=y`, `CONFIG_SECURE_BOOT=y`), and the
shipping BC01 2.0.3 image carries a valid signature block at offset
`0x270000`:

```
magic          : 0xe7, version 2 (RSA-PSS)
RSA modulus    : 3072 bits, exponent 65537
block CRC32    : valid
image digest   : matches SHA-256 of the signed region 0..0x270000
s^e mod n      : ends 0xbc, a well-formed PSS trailer
```

When Secure Boot is enabled, `esp_ota_set_boot_partition()` calls
`image_validate(ESP_IMAGE_VERIFY)`, which reaches
`verify_secure_boot_signature()` in `esp_image_format.c`. **A forged image
is rejected at OTA time**, before the boot partition is switched.

So the obfuscation being broken does not, by itself, yield code execution.

### Actual impact

Whether the signature is enforced depends on eFuses burned at manufacture,
which is independent of whether the binary was signed and is not observable
over the network.

**Measured on a retail BC01** running firmware 2.0.1, via
`esptool.py get_security_info`:

```
Secure Boot:      Enabled
  BLOCK_KEY0    - SECURE_BOOT_DIGEST0
  Secure Boot Key1 is Revoked
  Secure Boot Key2 is Revoked
Flash Encryption: Enabled
  SPI_BOOT_CRYPT_CNT: 0x7
  BLOCK_KEY1    - XTS_AES_128_KEY
JTAG:             Permanently Disabled
```

Secure Boot is enforced on that unit. **A forged OTA image is therefore a
denial of service, not code execution**: the attacker consumes the inactive
OTA slot and the bootloader refuses the result.

One unit is not the whole fleet, and the vendor describes this product as
pre-production, so other units may differ. Check your own — the command is
in [SECURE-BOOT.md](SECURE-BOOT.md). Where eFuses are *not* burned, the
signature is decorative and this finding becomes remote code execution.

### What Secure Boot does not cover

Pool configuration lives in NVS, not in the signed application partition.
No signature covers it. An unauthenticated attacker who repoints
`stratumURL` and `stratumUser` takes the device's entire hash output, and
the change survives reboot — on a fully locked-down unit exactly as on an
open one.

Secure Boot is the last line here, not the first. Finding 1 is the one that
matters, and it is untouched by any of this.

### Fix

Authentication now covers the OTA endpoint (finding 1), which is the
control that was actually missing.

The container format is deliberately left unchanged so that stock vendor
images still install. Replacing the XOR pad with real encryption would
break that compatibility while adding nothing: the integrity guarantee that
matters comes from Secure Boot, not from the transport. What the pad is
worth is documented honestly rather than overstated.

For units you flash yourself, enable Secure Boot v2 properly --
see [SECURE-BOOT.md](SECURE-BOOT.md).

---

## 3. Credentials shipped in the factory image

[`config.cvs`](../config.cvs) is flashed into the NVS partition at
manufacture. As released it contains:

| Key | Value | Problem |
|---|---|---|
| `wifissid` / `wifipass` | `GOKJ` / `GOKJ666888` | Factory test WiFi credentials in plaintext in every shipped image. |
| `influx_token` | `f37fh783hf8hq` | API token in plaintext. |
| `stratumuser` | `DG4GqZJ9gdNYvxg9ddYatByqzNKGtZJafx` | Payout address that belongs to neither the buyer nor, on its face, to a BTC wallet. |
| `fbstratumuser` | `LX1ysSCebjGSVPgf8Wqg2Tg9Ya1oRhHWsz` | Same, as fallback. |

A device flashed with the factory NVS and powered on before the owner
finishes configuring it mines to an address the owner does not control.

### Fix

`config.cvs` is replaced with a placeholder template carrying no
credentials and no payout address. Provisioning your own values is a
documented step in [README.md](../README.md), and the build fails rather
than silently flashing placeholders.

---

## 4. Uninitialised OTA handle used on error paths

In `POST_OTA_update()`:

```c
esp_ota_handle_t ota_handle;          /* declared, not initialised */
...
esp_ota_abort(ota_handle);            /* three error paths reach here ... */
...
esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);   /* ... before this */
```

Three failure paths — a short read on the header, a deobfuscation failure,
and a project-id mismatch — call `esp_ota_abort()` on an indeterminate
value before `esp_ota_begin()` ever assigns it. `esp_ota_abort()` looks the
handle up in its internal list, so the practical outcome is a spurious
`ESP_ERR_INVALID_ARG` in the common case, and corruption of an unrelated
in-flight OTA entry if the value happens to collide.

All three are reachable by an unauthenticated LAN client simply by
truncating a request.

### Fix

The handle is initialised to zero, aborts are guarded on a
`begun` flag, and the error paths that precede `esp_ota_begin()` no longer
call abort at all.

---

## 5. Telemetry enabled by default

`config.cvs` sets `influx_enable = 1` with `influx_url =
http://10.98.18.79`, a hardcoded private address, and a bucket and
organisation both named `nerdqaxeplus`.

The destination is unroutable outside the vendor's own network, so this
leaks nothing externally, but it means stock devices attempt periodic
unsolicited reporting by default over plain HTTP.

### Fix

Defaults to disabled. Enabling it is an explicit choice with a
user-supplied destination.

---

## Mitigations

If you are running stock Hammer firmware and are not yet ready to reflash:

1. **Put the miner on an isolated VLAN or a dedicated SSID** with no
   inbound access from general client devices. This is the single most
   effective step and it addresses findings 1 and 2 together.
2. **Do not expose the miner to the internet.** Never port-forward it. The
   private-range check is the only thing standing between the API and the
   open internet, and it fails open behind NAT-hairpin and some proxy
   setups.
3. **Change the payout address immediately** on any device you have not
   already configured, and confirm it in `/api/system/info`.
4. **Change the WiFi password** if your network ever used the factory
   `GOKJ` credentials.
5. **Treat the miner as an untrusted device on your network.** Assume any
   host that can reach it can control it.

---

## Keeping credentials out of this repository

An API password was committed here on 2026-08-27 as `.api-password.txt` and
stayed in a public repository until 2026-09-03. It was the live password on
two miners.

**The history was rewritten and it did not make the secret unrecoverable.**
That is the part worth internalising. After `git filter-repo` removed the file
from all 98 commits and the branch was force-pushed, the old commits still
resolved by SHA on GitHub, the blob API still returned its 22 bytes, and
`raw.githubusercontent.com` still served it at the old commit. GitHub keeps
unreachable objects until it garbage-collects, which it does on its own
schedule and not on request; every existing clone and fork kept the value too.

So the only fix that worked was **changing the password on the devices**.
Rewriting history is worth doing, but treat it as tidying up, not containment:
once a secret reaches a remote, it is compromised.

To have GitHub drop the unreachable objects, open a support request naming the
repository and asking them to run garbage collection. Do that *after*
rotating, not instead of it.

**Requested 2026-09-08.** Through the support portal's Virtual Agent, under
Repositories -- the "Clear cached views" button, not the "Deletes" category.
Deletes is the workflow for destroying a repository: it asks for the URL of the
repo you want removed and makes you confirm a purge that cannot be reversed. It
is not the route for cleaning up objects inside a live repository.

The agent asked for the dangling commit URL, whether it appears in a pull
request, and why the rewrite had not settled the matter. It then created a
ticket: "we'll update you once we've clear the cached views."

What was supplied, which is what GitHub's own documentation asks for:

| | |
|---|---|
| Repository | `SerpentXSF/StayOpen-Miner-Firmware`, public |
| Dangling commit | `26db064ef961c9c2dcf58b53071cb37651604b5f` |
| Rewritten to | `28dff91736c9b07c9310d8c61ef5756c82b81086` |
| Affected pull requests | 0 -- this repository has never had one |
| Forks | 0 |
| Orphaned LFS objects | none; LFS is not used here |

The first-changed-commit pair came from `.git/filter-repo/first-changed-commits`,
which the rewrite left sitting in the clone and which nobody had thought to
look at until it was asked for.

Confirmed still exposed at the moment of asking: the commit resolved through
the Git Commits API and its tree returned 19 entries including
`.api-password.txt`. To check whether it has since been collected:

```
gh api repos/SerpentXSF/StayOpen-Miner-Firmware/git/trees/26db064ef961c9c2dcf58b53071cb37651604b5f
```

A 404 means it is done.

**Outcome not yet recorded.** Note it here when GitHub replies, including if
they decline. Their agent opens by saying they assist "only in cases where we
determine sensitive data can not be mitigated by rotating affected
credentials", and this credential was rotated at the time, so a refusal is a
reasonable answer and belongs in the record as much as a success would.

### The hook

`.githooks/pre-commit` refuses to commit device credentials. Enable it once
per clone:

```bash
git config core.hooksPath .githooks
```

It refuses, against the staged content rather than the working tree:

- files that look like credentials by name (`*api-password*`, `*.pem`,
  signing keys)
- any `*.cvs` provisioning file that is not a `.example` template
- a filled-in `apipassword`, `wifipass`, `stratumpass`, `influx_token` or
  similar **in any file, whatever it is called** -- the earlier leak was in a
  file called `.api-password.txt`, but the next one need not be
- a `stratumuser` that is not the `REPLACE-WITH-YOUR-...` placeholder

The templates ship those keys with empty values, so they pass. Override with
`git commit --no-verify` when you genuinely mean it.

A hook is not a security boundary -- it runs only where it is enabled, and
`--no-verify` bypasses it. It is there to catch the accident, which is what
actually happened.

## Reporting

Security issues in *this* repository can be raised as a GitHub issue, or
privately if you prefer — see the repository contact details.

Issues in Hammer's shipping firmware belong with Hammer. The findings here
concern source the vendor published publicly and a protocol weakness
recoverable from a public download, so they are documented openly rather
than withheld; the reasoning is set out in
[OTA-FORMAT.md](OTA-FORMAT.md#why-this-is-documented-rather-than-withheld).
