#!/usr/bin/env python3
"""Build the artifacts published for other people to flash.

Produces, into dist/:

    stay-open-<board>-<ver>-full.bin    bootloader + tables + app + web UI
    stay-open-<board>-<ver>-app.bin     application only, for esptool at 0x20000
    stay-open-<board>-<ver>-www.bin     web UI only, for esptool at 0x9e0000
    stay-open-<board>-<ver>-ota.bin     wrapped for the miner's own updater
    stay-open-<board>-<ver>-www-ota.bin web UI, wrapped for the same updater
    manifest-<board>.json           for the esp-web-tools flasher
    SHA256SUMS

The NVS image inside the full build is generated from that board's
**config.<board>.cvs.example** (config.cvs.example for the BC01), never from a
local config.cvs. That distinction matters: config.cvs is flashed
verbatim and holds WiFi credentials and a payout address, so shipping one would
hand out the builder's network password and point other people's hashrate at
the builder's wallet. The published image therefore has no credentials in it,
and a freshly flashed miner comes up as an access point waiting to be told
where to mine.

Takes the board to publish, defaulting to bc01:

    python tools/make_release.py bc01

Run after `python tools/build_board.py <board>`, with the ESP-IDF environment
exported. The board is not cosmetic: it picks which build directory is read and
it goes in every filename. The boards do not share an ASIC pinout, and an image
flashed to the wrong one boots, serves its interface, detects the chip and
never returns a share -- so a mislabelled artifact is worse than a missing one.
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")


def layout(build):
    """offset, file -- must match partitions.csv"""
    return [
        (0x0, os.path.join(build, "bootloader", "bootloader.bin")),
        (0xD000, os.path.join(build, "partition_table", "partition-table.bin")),
        (0xE000, os.path.join(DIST, "config-dist.bin")),
        (0x16000, os.path.join(build, "ota_data_initial.bin")),
        (0x20000, os.path.join(build, "stayopen-miner.bin")),
        (0x9E0000, os.path.join(build, "www.bin")),
    ]


def version(build):
    """Read the version from the config the image was actually built with."""
    sdk = os.path.join(build, "sdkconfig")
    with open(sdk, encoding="utf-8", errors="replace") as fh:
        m = re.search(r'^CONFIG_APP_PROJECT_VER="([^"]+)"', fh.read(), re.M)
    return (m.group(1).split()[0] if m else "0.0.0")


def check_board(build, board):
    """Refuse to publish a build whose config is not the board being named.

    The filename is the only thing telling someone which board an image is
    for, and the cost of getting it wrong is a miner that looks broken. The
    build directory's own sdkconfig is the authority, so compare against it
    rather than trusting the directory name.
    """
    sdk = os.path.join(build, "sdkconfig")
    if not os.path.exists(sdk):
        sys.exit("no sdkconfig in %s -- run tools/build_board.py %s first"
                 % (build, board))
    with open(sdk, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = re.search(r'^CONFIG_DEVICE_MODULE="([^"]+)"', text, re.M)
    model = m.group(1).lower() if m else "?"
    if model != board.lower():
        sys.exit("build in %s is for %s, not %s -- refusing to publish it "
                 "under the wrong name" % (build, model, board))


def sh(cmd):
    print("  $", " ".join(cmd))
    subprocess.run(cmd, check=True, cwd=ROOT)


def config_template(board):
    """The provisioning template for this board.

    Per board, because the values are not interchangeable. A BC01's 120 is
    1.20 V for one ASIC domain; applied to a BC04 it is 1.20 V across four in
    series, 0.30 V each, and the board will not run. The BC01 keeps the
    historical filename; every other board gets config.<board>.cvs.example.
    """
    per_board = os.path.join(ROOT, "config.%s.cvs.example" % board)
    if os.path.exists(per_board):
        return per_board
    return os.path.join(ROOT, "config.cvs.example")


def check_template_board(src, board):
    """Refuse a template whose devicemodel is not the board being published.

    This used to read config.cvs.example unconditionally, so a BC04 release
    would have shipped a BC01 NVS: devicemodel BC01, 1.20 V across four
    domains, flipscreen upside down, and Ethernet left enabled on a board that
    stops passing packets the moment it powers its hashboard. The image would
    have flashed, booted, tripped the board-mismatch guard and never hashed.

    The filenames are checked elsewhere; this checks the settings inside.
    """
    model = None
    with open(src, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("devicemodel,"):
                parts = line.strip().split(",")
                if len(parts) >= 4:
                    model = parts[3].strip().lower()
                break
    if model is None:
        sys.exit("%s has no devicemodel row; refusing to publish"
                 % os.path.basename(src))
    if model != board:
        sys.exit("%s sets devicemodel %s but this is a %s release -- add a "
                 "config.%s.cvs.example rather than publishing another "
                 "board's settings"
                 % (os.path.basename(src), model.upper(), board.upper(), board))


def build_config(board):
    """NVS image from the example template, never from a local config.cvs."""
    idf = os.environ.get("IDF_PATH")
    if not idf:
        sys.exit("IDF_PATH is not set -- export the ESP-IDF environment first")
    gen = os.path.join(idf, "components", "nvs_flash",
                       "nvs_partition_generator", "nvs_partition_gen.py")
    src = config_template(board)
    out = os.path.join(DIST, "config-dist.bin")

    check_template_board(src, board)

    text = open(src, encoding="utf-8", errors="replace").read()
    if "REPLACE-WITH-YOUR-BTC-ADDRESS" not in text:
        sys.exit("%s has been edited to contain a real address; refusing to "
                 "publish it" % os.path.basename(src))
    print("provisioning from %s" % os.path.basename(src))
    sh([sys.executable, gen, "generate", src, out, "0x6000"])


def main():
    board = (sys.argv[1] if len(sys.argv) > 1 else "bc01").lower()
    build = os.path.join(ROOT, "build", board)

    check_board(build, board)
    ver = version(build)
    os.makedirs(DIST, exist_ok=True)
    print("building release artifacts for %s %s from %s" % (board, ver, build))

    build_config(board)

    LAYOUT = layout(build)
    for _, path in LAYOUT:
        if not os.path.exists(path):
            sys.exit("missing %s -- run tools/build_board.py %s first"
                     % (path, board))

    base = "stay-open-%s-%s" % (board, ver)
    full = os.path.join(DIST, base + "-full.bin")

    args = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge_bin",
            "-o", full, "--flash_mode", "dio", "--flash_size", "16MB",
            "--flash_freq", "80m"]
    for off, path in LAYOUT:
        args += [hex(off), path]
    sh(args)

    shutil.copy(os.path.join(build, "stayopen-miner.bin"),
                os.path.join(DIST, base + "-app.bin"))
    shutil.copy(os.path.join(build, "www.bin"),
                os.path.join(DIST, base + "-www.bin"))

    # Image the miner's own updater accepts. The container is obfuscated with
    # a repeating 16-byte XOR pad; this is the vendor's, recovered from a
    # shipped image without the key (docs/OTA-FORMAT.md). It is not a secret
    # and protects nothing -- it is here because the firmware's updater expects
    # the format, so an update built any other way is rejected.
    sh([sys.executable, os.path.join(ROOT, "tools", "ota_tool.py"), "pack",
        os.path.join(build, "stayopen-miner.bin"),
        "--pad", "69cc74aeaf0ce683229d422f54428a54",
        "-o", os.path.join(DIST, base + "-ota.bin")])

    # The web UI has its own updater, and it will not take the raw partition
    # image: it checks for a 0x55 type byte and rejects anything else with
    # "File error". Publishing only the raw www.bin left no way to update the
    # interface without a serial cable.
    sh([sys.executable, os.path.join(ROOT, "tools", "ota_tool.py"), "pack",
        os.path.join(build, "www.bin"),
        "--type", "www",
        "--pad", "69cc74aeaf0ce683229d422f54428a54",
        "-o", os.path.join(DIST, base + "-www-ota.bin")])

    # The parts an update needs, which is every region except the settings.
    #
    # Verified on a BC01 by writing exactly these five parts at these offsets,
    # which is what esp-web-tools does: the miner rejoined WiFi on its own
    # credentials, accepted its stored API password, and came back mining to
    # the same pool and worker. Every field compared identical -- SSID, WiFi
    # password, pool URL and port, payout address, fallback pool, frequency,
    # core voltage, hostname, auth.
    #
    # -full.bin is written at offset 0 and spans the whole flash, so it
    # necessarily includes nvs at 0xE000 -- and the nvs image inside it comes
    # from config.<board>.cvs.example. Flashing it to update therefore replaces
    # the owner's WiFi credentials, pool, payout address and API password with
    # placeholders. An owner did exactly that and their miner came back
    # authorising as REPLACE-WITH-YOUR-BTC-ADDRESS.
    #
    # Shipping the regions separately lets the flasher write all of them except
    # 0xE000, so settings survive. esp-web-tools takes any number of parts.
    update_parts = [
        (0x0,      os.path.join(build, "bootloader", "bootloader.bin"),        "bootloader"),
        (0xD000,   os.path.join(build, "partition_table", "partition-table.bin"), "partitions"),
        # 0xE000 is nvs, and is deliberately absent.
        (0x16000,  os.path.join(build, "ota_data_initial.bin"),                "otadata"),
        (0x20000,  os.path.join(build, "stayopen-miner.bin"),                  "app"),
        (0x9E0000, os.path.join(build, "www.bin"),                             "www"),
    ]
    parts_json = []
    for offset, src, tag in update_parts:
        name = "%s-%s.bin" % (base, tag)
        if tag not in ("app", "www"):       # those two are already in dist
            shutil.copy(src, os.path.join(DIST, name))
        parts_json.append({"path": name, "offset": offset})

    manifest = {
        "name": "SerpentX / Stay Open (%s)" % board.upper(),
        "version": ver,
        "home_assistant_domain": None,
        "new_install_prompt_erase": False,
        "builds": [{
            "chipFamily": "ESP32-S3",
            "parts": [{"path": base + "-full.bin", "offset": 0}],
        }],
    }

    # A second manifest for the same release, keeping the settings.
    update_manifest = {
        "name": "SerpentX / Stay Open (%s) -- update, keeps settings" % board.upper(),
        "version": ver,
        "home_assistant_domain": None,
        # Erasing is exactly what this manifest exists to avoid.
        "new_install_prompt_erase": False,
        "builds": [{
            "chipFamily": "ESP32-S3",
            "parts": parts_json,
        }],
    }
    with open(os.path.join(DIST, "manifest-%s-update.json" % board), "w",
              encoding="utf-8", newline="\n") as fh:
        json.dump(update_manifest, fh, indent=2)
        fh.write("\n")
    # Per board. One manifest.json could only ever describe one board, so
    # publishing a second silently replaced the first and the flasher offered
    # whichever was released last under both names.
    manifest_name = "manifest-%s.json" % board
    with open(os.path.join(DIST, manifest_name), "w",
              encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, indent=2)
        fh.write("\n")

    lines = []
    for name in sorted(os.listdir(DIST)):
        if name in ("SHA256SUMS", "config-dist.bin") or not name.endswith(
                (".bin", ".json")):
            continue
        with open(os.path.join(DIST, name), "rb") as fh:
            lines.append("%s  %s\n" % (hashlib.sha256(fh.read()).hexdigest(),
                                       name))
    with open(os.path.join(DIST, "SHA256SUMS"), "w",
              encoding="utf-8", newline="\n") as fh:
        fh.writelines(lines)

    os.remove(os.path.join(DIST, "config-dist.bin"))
    print("\ndist/ contains:")
    for line in lines:
        print("  " + line.strip())


if __name__ == "__main__":
    main()
