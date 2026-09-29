#!/usr/bin/env python3
"""Watch a miner over the network for days and record what happened when it died.

    python tools/soak_monitor.py 192.168.50.223
    python tools/soak_monitor.py 192.168.50.223 --hours 48 --interval 30
    python tools/soak_monitor.py 1.2.3.4 --password-file ../secrets/bc04-api-password.txt

Why this exists
---------------
Two BC04s have failed the same way -- the whole I2C domain gone, the controller
still healthy -- and both times the useful evidence was whatever happened to be
on screen or in a serial buffer at the moment someone looked. The second one was
running the vendor's own firmware, so nothing here may assume ours.

The point is not the numbers while it is healthy. It is that if a board dies at
3am there is a file holding the last good sample, the first bad one, and the gap
between them.

On probing
----------
Endpoint discovery runs ONCE, at startup. An earlier attempt at poking a stock
BC04's HTTP API from a script filled that owner's log with 404s and taught us
nothing, so this tries a short candidate list a single time and afterwards only
ever calls the one that answered.

If nothing answers -- likely on stock THOR, whose web bundle calls /api/claw/*
endpoints that did not respond when probed -- it falls back to a TCP reachability
check. "Stopped answering at 04:12" is still the fact worth having.
"""

import argparse
import csv
import json
import os
import socket
import sys
import time
import urllib.error
import urllib.request

# A provider is the set of endpoints one firmware serves, plus the conversion
# from its field names and units into the canonical ones below. Discovery tries
# each provider's probe path once and keeps the first that answers.
#
# Units are normalised so one CSV compares across firmwares: hashrate in GH/s,
# voltages and currents in millivolts and milliamps, matching our own API.


def _thor(payloads):
    """Stock Hammer THOR, /v2. Discovered by watching its own web UI, since
    blind probing an owner's miner is how you fill their log with 404s."""
    d = {}
    for body in payloads:
        if isinstance(body, dict) and isinstance(body.get("data"), dict):
            d.update(body["data"])

    def scale(key, factor):
        v = d.get(key)
        return None if v is None else v * factor

    return {
        "uptimeSeconds": d.get("uptime_seconds"),
        "hashRate": scale("current_hashrate", 1e-9),      # H/s -> GH/s
        "temp": d.get("temp_board"),
        "vrTemp": d.get("temp_vcore"),
        "coreVoltageActual": scale("core_voltage_actual", 1000.0),
        "coreVoltage": d.get("coreVoltage"),
        "frequency": d.get("frequency"),
        "power": d.get("power_consumption"),
        "voltage": scale("input_voltage", 1000.0),
        "current": scale("core_current_actual", 1000.0),
        "fanrpm": d.get("fan_speed_rpm"),
        "fanspeed": d.get("fan_target_speed"),
        "sharesAccepted": d.get("shares_accepted"),
        "sharesRejected": d.get("shares_rejected"),
        # The count the vendor itself reports. This going to zero is the whole
        # reason this script exists.
        "asicCount": d.get("detected_chips_count"),
        "asicDetected": d.get("detected_chips_count"),
        "wifiRSSI": d.get("wifi_rssi"),
        "freeHeap": d.get("free_heap"),
        "hwErrorCount": d.get("nonce_mismatch_errors"),
        "uartCrcErrors": d.get("uart_crc_errors"),
        "queueDropErrors": d.get("queue_drop_errors"),
        "staleShareErrors": d.get("stale_share_errors"),
        "ethLinkUp": d.get("eth_link_up"),
        "bootMode": d.get("boot_mode"),
    }


def _ours(payloads):
    """This firmware: one endpoint, already in canonical names and units."""
    body = payloads[0] if payloads else None
    return dict(body) if isinstance(body, dict) else {}


PROVIDERS = [
    ("stay-open", ["/api/system/info"], _ours),
    ("thor-v2", ["/v2/device/status", "/v2/miner/status", "/v2/device/info"], _thor),
]

# Recorded when present. A miner that does not report one leaves the cell empty,
# which is why every board can share one file format.
FIELDS = [
    "uptimeSeconds", "hashRate", "temp", "temp2", "vrTemp", "coreVoltageActual",
    "coreVoltage", "frequency", "power", "voltage", "current", "fanrpm",
    "fanrpm0", "fanrpm1", "fanspeed", "sharesAccepted", "sharesRejected",
    "hwErrorCount", "asicCount", "asicDetected", "wifiRSSI", "systemError",
    "power_fault", "overheat_mode",
    # The reason a soak runs for a day rather than an hour. Everything else
    # here shows the miner is working now; free heap is the one figure that
    # shows whether it will still be working tomorrow, because a slow leak
    # looks exactly like a healthy miner until it does not. Left out of the
    # first three soaks, which is why none of them can answer the question
    # they were run to answer.
    "freeHeap",
    # Stock THOR reports these and ours does not; they stay empty on ours.
    "uartCrcErrors", "queueDropErrors", "staleShareErrors", "ethLinkUp",
    "bootMode",
]


def http_get(url, token, timeout=10):
    req = urllib.request.Request(url)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read()


def login(host, password, log):
    """Authenticate if the miner wants it. Ours does; stock may not."""
    if not password:
        return None
    try:
        req = urllib.request.Request(
            "http://%s/api/system/login" % host,
            data=json.dumps({"password": password}).encode())
        req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=10) as r:
            token = json.loads(r.read()).get("token")
        log("authenticated")
        return token
    except Exception as exc:
        log("login failed (%s) -- carrying on unauthenticated" % type(exc).__name__)
        return None


def discover(host, token, log):
    """Find which firmware this is. Once, and never again."""
    for name, paths, _ in PROVIDERS:
        try:
            status, body = http_get("http://%s%s" % (host, paths[0]), token)
            if status == 200:
                json.loads(body)
                log("firmware looks like '%s'; polling %s"
                    % (name, ", ".join(paths)))
                return name
        except urllib.error.HTTPError as exc:
            log("  %s -> HTTP %s" % (paths[0], exc.code))
        except Exception as exc:
            log("  %s -> %s" % (paths[0], type(exc).__name__))
    log("nothing recognised; falling back to a reachability check only. "
        "Restarts and hardware faults will NOT be visible -- only whether it "
        "is still there.")
    return None


def sample(host, provider, token):
    """One reading, canonical. Raises if the first endpoint cannot be reached."""
    paths, normalise = next((p, n) for nm, p, n in PROVIDERS if nm == provider)
    payloads = []
    for i, path in enumerate(paths):
        try:
            status, body = http_get("http://%s%s" % (host, path), token)
            payloads.append(json.loads(body) if status == 200 else None)
        except Exception:
            if i == 0:
                raise       # the miner is unreachable, not merely partial
            payloads.append(None)
    return normalise([p for p in payloads if p is not None])


def tcp_alive(host, port=80, timeout=5):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def num(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("host")
    parser.add_argument("--hours", type=float, default=48.0)
    parser.add_argument("--interval", type=float, default=30.0, help="seconds")
    parser.add_argument("--password-file", default=None)
    parser.add_argument("--out", default=None, help="directory for the log files")
    args = parser.parse_args()

    out_dir = args.out or os.path.join(
        os.getcwd(), "soak-%s" % args.host.replace(".", "-"))
    os.makedirs(out_dir, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    csv_path = os.path.join(out_dir, "telemetry-%s.csv" % stamp)
    log_path = os.path.join(out_dir, "events-%s.log" % stamp)

    log_fh = open(log_path, "a", encoding="utf-8")

    def log(msg):
        line = "[%s] %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
        print(line, flush=True)
        log_fh.write(line + "\n")
        log_fh.flush()      # a crash at 3am must not cost us the last line

    password = None
    if args.password_file:
        try:
            with open(args.password_file, encoding="utf-8") as fh:
                password = fh.read().strip()
        except OSError as exc:
            log("could not read the password file: %s" % exc)

    log("watching %s for %.1f h, sampling every %.0f s"
        % (args.host, args.hours, args.interval))
    log("telemetry -> %s" % csv_path)

    token = login(args.host, password, log)
    provider = discover(args.host, token, log)

    csv_fh = open(csv_path, "a", newline="", encoding="utf-8")
    writer = csv.writer(csv_fh)
    writer.writerow(["time", "reachable"] + FIELDS)
    csv_fh.flush()

    deadline = time.time() + args.hours * 3600
    prev = {}
    baseline = {}
    consecutive_fail = 0
    samples = 0
    settle_after = max(1, int(600 / args.interval))

    while time.time() < deadline:
        now = time.strftime("%Y-%m-%d %H:%M:%S")
        data = None
        reachable = False

        if provider:
            try:
                data = sample(args.host, provider, token)
                reachable = True
            except urllib.error.HTTPError as exc:
                # Sessions live in RAM, so a restart invalidates them. A 401
                # after a good run is itself a hint that it rebooted.
                if exc.code == 401 and password:
                    log("401 -- re-authenticating (it may have restarted)")
                    token = login(args.host, password, log)
                reachable = True        # it answered, just not with data
            except Exception:
                reachable = tcp_alive(args.host)
        else:
            reachable = tcp_alive(args.host)

        row = [now, int(reachable)]
        cur = {}
        for field in FIELDS:
            value = data.get(field) if isinstance(data, dict) else None
            cur[field] = value
            row.append("" if value is None else value)
        writer.writerow(row)
        csv_fh.flush()

        if not reachable:
            consecutive_fail += 1
            if consecutive_fail in (1, 3, 10) or consecutive_fail % 60 == 0:
                log("UNREACHABLE (%d in a row, about %.0f s)"
                    % (consecutive_fail, consecutive_fail * args.interval))
        else:
            if consecutive_fail >= 3:
                log("back after %d missed samples (about %.0f s)"
                    % (consecutive_fail, consecutive_fail * args.interval))
            consecutive_fail = 0

        if data:
            samples += 1

            # A restart shows as uptime going backwards. Said loudly, because
            # both failures so far involved the board restarting or stopping
            # with nobody watching.
            up = num(cur.get("uptimeSeconds"))
            prev_up = num(prev.get("uptimeSeconds"))
            if up is not None and prev_up is not None and up < prev_up:
                log("RESTARTED -- uptime went %.0fs -> %.0fs" % (prev_up, up))
                baseline = {}       # whatever it settles at now is the new normal
                samples = 0

            # A fault the firmware itself is reporting. Only ours fills this in.
            fault = cur.get("systemError")
            if fault and fault != prev.get("systemError"):
                log("FAULT REPORTED: %s" % fault)
            if cur.get("power_fault") and cur.get("power_fault") != prev.get("power_fault"):
                log("POWER FAULT: %s" % cur.get("power_fault"))
            if cur.get("overheat_mode") and not prev.get("overheat_mode"):
                log("OVERHEAT MODE")

            # The specific shape of both BC04 deaths: the hashboard stops being
            # visible while the controller carries on answering perfectly.
            detected = num(cur.get("asicDetected"))
            prev_detected = num(prev.get("asicDetected"))
            if detected is not None and prev_detected is not None and detected < prev_detected:
                log("ASICS LOST -- detected went %.0f -> %.0f" % (prev_detected, detected))
            for probe, label in (("vrTemp", "regulator temperature"),
                                 ("coreVoltageActual", "core voltage"),
                                 ("power", "power")):
                value = num(cur.get(probe))
                prev_value = num(prev.get(probe))
                if value is not None and prev_value is not None and prev_value > 0 and value == 0:
                    log("%s DROPPED TO ZERO (was %.2f) -- this is what a lost "
                        "hashboard looks like" % (label.upper(), prev_value))

            # Let it settle before judging it, so a warm-up is never mistaken
            # for a fault. Ten minutes at the default interval.
            if samples == settle_after:
                for key in ("hashRate", "temp", "vrTemp", "power"):
                    value = num(cur.get(key))
                    if value is not None:
                        baseline[key] = value
                if baseline:
                    log("baseline: " + ", ".join(
                        "%s=%.1f" % (k, v) for k, v in baseline.items()))

            if baseline:
                rate = num(cur.get("hashRate"))
                base_rate = baseline.get("hashRate")
                if rate is not None and base_rate and base_rate > 0 and rate < base_rate * 0.7:
                    log("HASHRATE LOW: %.1f against a baseline of %.1f" % (rate, base_rate))
                for key, limit, label in (("temp", 10.0, "board temperature"),
                                          ("vrTemp", 10.0, "regulator temperature")):
                    value = num(cur.get(key))
                    base = baseline.get(key)
                    if value is not None and base is not None and value - base > limit:
                        log("%s UP %.1f over baseline (%.1f -> %.1f)"
                            % (label.upper(), value - base, base, value))

            prev = cur

        time.sleep(args.interval)

    log("finished; telemetry in %s" % csv_path)
    csv_fh.close()
    log_fh.close()


if __name__ == "__main__":
    main()
