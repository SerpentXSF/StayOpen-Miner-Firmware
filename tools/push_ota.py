#!/usr/bin/env python3
"""Push a packed update image to a running miner, over the network.

    python tools/push_ota.py <host> <password-file> <image.bin> [www]

The other half of ota_tool.py. That packs a container; this delivers one.
Until now there was nothing in the tree that did the second part, so every
over-the-air update meant driving the web UI by hand -- fine once, tedious
across a day of them, and impossible to put in a script.

The image must already be packed. A raw application binary is not an update
image: the miner reads a type byte and an obfuscated header first and rejects
anything else. See docs/OTA-FORMAT.md, and ota_tool.py pack.

    python tools/ota_tool.py pack build/bc04/stayopen-miner.bin \\
        -o /tmp/update.bin --pad <hex>
    python tools/push_ota.py <host> ../secrets/bc04-api-password.txt \\
        /tmp/update.bin

The web UI lives in its own partition behind its own endpoint, so a 0x55
container needs the `www` argument:

    python tools/ota_tool.py pack build/bc04/www.bin -o /tmp/www.bin \\
        --type www --pad <hex>
    python tools/push_ota.py <host> <password-file> /tmp/www.bin www

The password is read from a file rather than taken on the command line, so it
stays out of the shell history and out of the process list. Nothing here
prints it, or the token it is exchanged for.
"""
import json
import sys
import time
import urllib.error
import urllib.request

LOGIN_TIMEOUT_S = 15

# Long enough for a 2.8 MB image over WiFi on a busy miner. The upload
# competes with hashing, the web UI and two Stratum sessions.
UPLOAD_TIMEOUT_S = 300


def login(host, password_file):
    """Exchange the stored password for a bearer token."""
    with open(password_file, encoding="utf-8") as handle:
        password = handle.read().strip()

    request = urllib.request.Request(
        "http://%s/api/system/login" % host,
        data=json.dumps({"password": password}).encode())
    request.add_header("Content-Type", "application/json")

    with urllib.request.urlopen(request, timeout=LOGIN_TIMEOUT_S) as response:
        return json.loads(response.read().decode()).get("token")


def load_image(path, kind):
    """Read the container and check it is the kind being asked for.

    Before the network, deliberately. Packing the wrong thing is the easy
    mistake to make -- a raw application binary, or a www container aimed at
    the application endpoint -- and finding out needs no miner at all, so it
    should not need a reachable one.
    """
    with open(path, "rb") as handle:
        image = handle.read()

    if not image or image[0] not in (0xAA, 0x55):
        sys.exit("%s does not start with a container type byte; pack it first "
                 "with ota_tool.py (see this file's docstring)" % path)
    if kind == "www" and image[0] != 0x55:
        sys.exit("that is an application image (0xAA); the www endpoint wants "
                 "a 0x55 container")
    if kind != "www" and image[0] != 0xAA:
        sys.exit("that is a web-UI image (0x55); pass 'www' to send it to the "
                 "right endpoint")
    return image


def push(host, token, path, image, kind):
    endpoint = "OTAWWW" if kind == "www" else "OTA"

    print("pushing %s (%d bytes) to %s" % (path, len(image), host))
    request = urllib.request.Request("http://%s/api/system/%s" % (host, endpoint),
                                     data=image)
    request.add_header("Content-Type", "application/octet-stream")
    request.add_header("Authorization", "Bearer " + token)
    request.add_header("Content-Length", str(len(image)))

    started = time.time()
    try:
        with urllib.request.urlopen(request, timeout=UPLOAD_TIMEOUT_S) as response:
            print("HTTP %s: %s" % (response.status,
                                   response.read()[:200].decode("utf-8", "replace").strip()))
    except urllib.error.HTTPError as exc:
        print("HTTP %s: %s" % (exc.code,
                               exc.read()[:300].decode("utf-8", "replace").strip()))
        return 1
    except TimeoutError as exc:
        # A timeout is not a reboot, and it used to be reported as one.
        #
        # Everything that was not an HTTPError fell into one handler that
        # printed "this is normal, the miner is rebooting" and returned 0. So
        # an upload that ran the full UPLOAD_TIMEOUT_S without finishing --
        # a miner that stopped reading, a WiFi link that went away mid-image
        # -- exited successfully with a reassuring message. In a script that
        # is worse than a crash: the next step runs believing the firmware
        # changed, and `push_ota.py app && push_ota.py www` would push the web
        # UI of a version that never installed.
        print("TIMED OUT after %.0fs: %r" % (time.time() - started, exc))
        print("The miner stopped accepting the upload before it finished, so "
              "nothing was installed. It may be partway through writing an "
              "OTA slot; check the version it reports before pushing again.")
        return 1
    except urllib.error.URLError as exc:
        # A timeout during the connect, rather than during the transfer,
        # arrives wrapped in a URLError instead of raised directly.
        if isinstance(exc.reason, TimeoutError):
            print("TIMED OUT after %.0fs: %r" % (time.time() - started, exc))
            print("The miner did not accept the connection, so nothing was "
                  "uploaded.")
            return 1
        print("connection ended: %r -- this is normal if the whole image went "
              "out, because the miner restarts as soon as it has it" % (exc,))
    except Exception as exc:
        # The miner restarts the moment it has the whole image, so the
        # connection dropping at the end is the expected outcome and not a
        # failure. Saying so here saves the next person a diagnosis.
        print("connection ended: %r -- this is normal if the whole image went "
              "out, because the miner restarts as soon as it has it" % (exc,))

    elapsed = time.time() - started
    print("took %.0fs for %d bytes" % (elapsed, len(image)))
    # Said plainly rather than implied: this script cannot tell a completed
    # upload from one that was cut short, because the drop looks the same
    # either way from this end. The miner's own version is the answer.
    print("Confirm the miner's reported version before treating this as done; "
          "a dropped connection looks identical from here whether the image "
          "arrived or not.")
    return 0


def main(argv):
    if len(argv) < 4 or len(argv) > 5:
        sys.stderr.write(__doc__)
        return 2

    host, password_file, path = argv[1], argv[2], argv[3]
    kind = argv[4] if len(argv) == 5 else "app"
    if kind not in ("app", "www"):
        sys.exit("the fourth argument, if given, must be 'www'")

    image = load_image(path, kind)

    token = login(host, password_file)
    if not token:
        sys.exit("login did not return a token; wrong password file?")
    print("login: ok")

    return push(host, token, path, image, kind)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
