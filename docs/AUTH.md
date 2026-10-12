# API authentication

How the HTTP API is protected, and what to do about devices that predate it.

---

## What the vendor firmware did

Nothing. The web UI shipped a login page that POSTed to
`/api/system/login`, but that route was never registered in the firmware.
No endpoint checked a credential.

The only gate was `is_network_allowed()`, which asks whether the caller's
address is in an RFC1918 range. That is a DNS-rebinding mitigation. It
answers "is this request from the local network", not "is this request
from someone allowed to make it", so every host on the LAN had full
administrative access — including the ability to change the payout address
and start a firmware update.

See [SECURITY.md](SECURITY.md#1-no-authentication-on-any-endpoint).

---

## What this firmware does

A shared secret, stored in NVS under `apipassword`, exchanged for a session
token.

```
POST /api/system/login
Content-Type: application/json

{"password": "your-password"}
```

```json
{ "token": "3f9a…64 hex characters" }
```

Every subsequent request carries it:

```
Authorization: Bearer 3f9a…
```

This is the endpoint and response shape the vendor's own web UI was already
written against, so the existing login page works against it unchanged.

### Properties

| | |
|---|---|
| Token | 32 bytes from `esp_fill_random()`, hex encoded |
| Concurrent sessions | 4, least-recently-used evicted |
| Idle timeout | 12 hours |
| Failed-attempt lockout | 60 seconds after 8 consecutive failures |
| Password comparison | Length-independent of content; see below |

Password and token comparisons run over the full length and accumulate
differences rather than returning at the first mismatch, so they do not
leak the length of a correct prefix through timing. The *length* of the
password still leaks. Buffers holding the secret are zeroed before the
functions holding them return.

### What is protected

Every API endpoint: `/api/system` (`PATCH`), `/api/system/restart`,
`/api/system/pool/drop`, `/api/system/OTA`, `/api/system/OTAWWW`,
`/api/system/info`, `/api/system/log/download`, `/api/get_*`,
`/api/set_network`, `/api/influx*`, `/api/sync_time`, and the `/api/ws`
websocket.

Deliberately left open:

- **`OPTIONS` preflight** — browsers do not send `Authorization` on a CORS
  preflight, so requiring it there would break the UI without adding
  anything; a preflight discloses nothing.
- **Static files and `/recovery`** — the login page has to be reachable
  before anyone can log in. The recovery page is only a form; the endpoint
  it posts to is authenticated like any other.

---

## Devices with no password set

**If `apipassword` is empty, authentication is disabled and the API behaves
exactly as the vendor firmware did.** The firmware logs a warning at every
boot, and `/api/system/info` reports `"authEnabled": false` so the UI can
say so.

This is a deliberate compromise, and it is worth being explicit about why.
Enforcing a password on a device that has never had one would lock the
owner out of their own miner with no way in short of a serial reflash. A
device that upgrades to this firmware keeps working; the owner sets a
password when they choose.

The consequence is that **upgrading alone does not fix finding 1.** You
have to set a password.

```bash
curl -X PATCH http://<miner>/api/system \
     -H 'Content-Type: application/json' \
     -d '{"apiPassword":"choose-something-long"}'
```

Or set `apipassword` in `config.cvs` before flashing. Changing the password
revokes every existing token.

Until then, treat network isolation as the only control — see
[SECURITY.md](SECURITY.md#mitigations).

---

## What this is not

The miner speaks plain HTTP. There is no TLS on the local API, so the
password and the token both cross the network in the clear, and anyone able
to observe traffic between you and the miner can replay them.

This raises the bar from "anyone who can route a packet to the device" to
"anyone who can observe traffic to the device or knows the password". On a
normal switched home or shop network that is a real improvement. It is not
a substitute for keeping the miner off untrusted networks, and it is not
protection against an attacker already positioned on the path.

The honest summary: this closes the hole where no credential existed at
all. Treat the miner as an appliance on a trusted segment regardless.

## The websocket is authenticated differently

The log stream at `/api/ws` takes its token from the query string
(`/api/ws?token=...`) as well as from the `Authorization` header.

A browser `WebSocket` cannot send custom headers -- the API has no facility for
it -- so gating that endpoint on a bearer header rejected every connection the
log viewer made. The failure was confusing to read: the page reported
"connection successful" and then an error, because the server upgrades the
socket before the handler runs, so by the time the request is rejected there is
no HTTP response left to send. The connection is closed instead.

A token in a URL is worse than one in a header. It lands in browser history and
in the logs of anything on the path. For a miner reached over a LAN that is a
reasonable trade for having the log stream work, and it is the choice most
embedded interfaces make -- but it is a trade, not a free win. The token is
still the same short-lived session token, it still expires, and clearing the
password still invalidates it.
