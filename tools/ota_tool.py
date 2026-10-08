#!/usr/bin/env python3
"""Pack, unpack and inspect Hammer miner OTA update images.

The update container and the obfuscation it uses are described in
docs/OTA-FORMAT.md. In short: a 0xAA type byte, a 48-byte header, then the
payload, with header and payload XORed against a repeating 16-byte pad
derived from a build-time key.

The pad can be recovered from a single image with no key, because the
container carries enough known plaintext to reveal every byte of it -- see
`recover`. Recovery states what it could and could not prove, and `inspect`
will not pass judgement on an image it could not decode.

    python tools/ota_tool.py inspect  update.bin
    python tools/ota_tool.py unpack   update.bin -o app.bin
    python tools/ota_tool.py pack     app.bin    -o update.bin --pad <hex>
    python tools/ota_tool.py recover  update.bin
    python tools/ota_tool.py selftest dist/*-ota.bin
"""

import argparse
import collections
import hashlib
import os
import struct
import sys
import zlib

# Application images are tagged 0xAA and web-UI images 0x55; the firmware
# rejects the wrong one outright. The APP payload is obfuscated with the
# pad, the WWW payload is not -- see docs/OTA-FORMAT.md.
TYPE_APP = 0xAA
TYPE_WWW = 0x55
HEADER_SIZE = 48
PAD_SIZE = 16
PROJECT_ID = b"GULLPOWER"
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432


def xor_pad(data, pad, offset=0):
    """Apply the repeating pad, continuing the keystream from `offset`."""
    return bytes(b ^ pad[(offset + i) % PAD_SIZE] for i, b in enumerate(data))


def pad_from_key(key):
    """Derive the pad the firmware would use for a 32-byte key."""
    if len(key) != 32:
        raise ValueError(f"key must be 32 bytes, got {len(key)}")
    return hashlib.sha256(key).digest()[:PAD_SIZE]


def recover_pad_by_frequency(payload, plain_byte=0x00):
    """Recover the pad by assuming the commonest plaintext byte per column.

    This was the only recovery method, and it is wrong for these images. It
    is kept because it is the one thing that can be tried against a container
    whose project id is not ours, and it is never used unless asked for: its
    answer cannot be checked, and an unverifiable pad is how a good release
    came to be reported as corrupt. See the note above known_plaintext().
    """
    pad = bytearray(PAD_SIZE)
    for index in range(PAD_SIZE):
        column = payload[index::PAD_SIZE]
        most_common = collections.Counter(column).most_common(1)[0][0]
        pad[index] = most_common ^ plain_byte
    return bytes(pad)


# esp_app_desc_t's reserv1: two uint32s that ESP-IDF zero-fills, at this
# offset into an application payload. Verified zero on every app container in
# dist/ and dist/archive, 2.0.19 through 2.0.31-dev.
#
# Its neighbour reserv2 at [208:288] looks like a fourth source and is not:
# measured on 2.0.31-dev it is not zero-filled.
APP_RESERV1 = (40, 48)

KnownByte = collections.namedtuple("KnownByte", "index value source from_payload")


def known_plaintext(blob):
    """Every pad byte the container gives away about itself.

    Recovery used to be frequency analysis: assume the commonest plaintext
    byte in each column modulo the pad length is 0x00 and read the pad off
    the ciphertext. That assumption is false here and it fails silently.
    Measured on stay-open-bc04-2.0.31-dev-ota.bin, the commonest aligned
    16-byte plaintext block is

        63 08 63 08 63 08 63 08 63 08 63 08 63 08 63 08   x3359

    with 0864 and 0884 next, so the commonest byte per column is
    00 08 00 08 ... and the recovered pad came back with bit 3 flipped at
    every odd index. `inspect` then decoded the header through that pad,
    printed the project id as 'G]LDPGWMR\x08' and a declared length of
    137061568 against an actual 2845888, and called it `integrity:
    MISMATCH` -- on a release miners accept and install. A tool reached for
    when an update has gone wrong must not report a good image as corrupt.

    Known plaintext settles it instead of guessing. The header is XORed from
    keystream offset 0 and the payload from HEADER_SIZE, which is a whole
    number of pad lengths, so in both cases byte n of a region uses
    pad[(region_start + n) % PAD_SIZE]:

        project id      header[32:42]   a constant, NUL-padded   -> pad[0:10]
        payload length  header[42:46]   the file's own size      -> pad[10:14]
        reserv1         payload[40:48]  zeroed in esp_app_desc_t -> pad[8:16]
        payload digest  header[0:32]    www payloads are plain   -> pad[0:16]

    They overlap, so a wrong assumption surfaces as a conflict rather than as
    a quiet wrong answer.

    The last two are what reach pad[14:16], and exactly one of them applies
    to any given container: an application payload is obfuscated, so its
    digest cannot be known until the pad is, while a www payload travels in
    the clear, so its digest is known without the pad and the header's digest
    field becomes thirty-two bytes of known plaintext covering every pad byte
    twice.

    Reaching those last two bytes matters more than it looks: header[0:32] is
    the digest field and header byte n uses pad[n % 16], so leaving two pad
    bytes unestablished garbles four bytes of the decoded digest -- enough on
    its own to make an intact container read as MISMATCH.

    The first two sources hold whatever the payload turns out to be; the last
    two are only as good as the payload is intact. `from_payload` marks that
    difference and recover_pad() ranks them below, so damage to a payload
    cannot unseat a pad byte the header has already settled.
    """
    header_ct, payload_ct = split(blob)
    found = []

    project = bytearray(10)
    project[0:len(PROJECT_ID)] = PROJECT_ID
    for n, plain in enumerate(project):
        offset = 32 + n
        found.append(KnownByte(offset % PAD_SIZE,
                               header_ct[offset] ^ plain, "project id", False))

    for n, plain in enumerate(struct.pack("<I", len(payload_ct))):
        offset = 42 + n
        found.append(KnownByte(offset % PAD_SIZE,
                               header_ct[offset] ^ plain, "payload length", False))

    start, end = APP_RESERV1
    if blob[0] == TYPE_APP and len(payload_ct) >= end:
        for offset in range(start, end):
            found.append(KnownByte((HEADER_SIZE + offset) % PAD_SIZE,
                                   payload_ct[offset] ^ 0x00,
                                   "esp_app_desc_t reserv1", True))

    if blob[0] == TYPE_WWW:
        for n, plain in enumerate(hashlib.sha256(payload_ct).digest()):
            found.append(KnownByte(n % PAD_SIZE,
                                   header_ct[n] ^ plain, "payload digest", True))
    return found


def recover_pad(blob):
    """Recover the pad from known plaintext and report what went unproven.

    Returns (pad, missing, conflicts). `missing` holds pad indices nothing
    established; `conflicts` holds (index, {value: sources}) where sources of
    equal standing disagreed. Either one means part of the pad is a guess,
    and callers are expected to say so rather than quietly decode through it.

    The payload-independent sources decide. The payload-derived ones are used
    only for what is left over, and only once they have agreed with the
    others everywhere the two overlap -- a source that is wrong about a byte
    we can check has not earned the bytes we cannot.
    """
    firm, loose = {}, {}
    for byte in known_plaintext(blob):
        bucket = loose if byte.from_payload else firm
        bucket.setdefault(byte.index, {}).setdefault(byte.value, set()).add(byte.source)

    pad, missing, conflicts = bytearray(PAD_SIZE), [], []

    def settle(index, seen):
        if len(seen) > 1:
            conflicts.append((index, {v: sorted(s) for v, s in seen.items()}))
            pad[index] = min(seen)
            return False
        pad[index] = next(iter(seen))
        return True

    for index, seen in firm.items():
        settle(index, seen)

    # Does the payload agree about the bytes the header already settled?
    overlap = [i for i in loose if i in firm and not conflicts]
    disagreed = [i for i in overlap if set(loose[i]) != set(firm[i])]

    for index in range(PAD_SIZE):
        if index in firm:
            continue
        if index in loose and not disagreed:
            settle(index, loose[index])
        else:
            missing.append(index)

    if disagreed:
        sources = sorted({s for i in disagreed for v in loose[i] for s in loose[i][v]})
        conflicts.append((None, {
            "the payload disagrees with the header at %d of %d checkable bytes"
            % (len(disagreed), len(overlap)): sources}))

    return bytes(pad), missing, conflicts


def validate_pad(blob, pad):
    """Check a pad against invariants that do not involve the payload digest.

    Staying independent of the digest is the whole point: it is what lets
    `inspect` tell "this pad is wrong" from "this image is corrupt", instead
    of reporting the second every time the first is true.

    Returns a list of complaints; empty means the pad decodes the header to
    something self-consistent.
    """
    header_ct, payload_ct = split(blob)
    header = parse_header(xor_pad(header_ct, pad, 0))

    problems = []
    if header["project_id"] != PROJECT_ID:
        problems.append("project id decodes to %r, expected %r"
                        % (header["project_id"], PROJECT_ID))
    if header["length"] != len(payload_ct):
        problems.append("header declares %d payload bytes, the file carries %d"
                        % (header["length"], len(payload_ct)))
    return problems


def split(blob):
    """Split a container into (header_ciphertext, payload_ciphertext)."""
    if len(blob) < 1 + HEADER_SIZE:
        raise ValueError("file is too short to be an update image")
    if blob[0] not in (TYPE_APP, TYPE_WWW):
        raise ValueError(f"bad type flag 0x{blob[0]:02x}, expected 0x{TYPE_APP:02x} (app) "
                         f"or 0x{TYPE_WWW:02x} (www)")
    return blob[1:1 + HEADER_SIZE], blob[1 + HEADER_SIZE:]


def parse_header(header):
    """Decode a decrypted 48-byte header."""
    return {
        "sha256": header[:32],
        "project_id": header[32:42].split(b"\0")[0],
        "length": struct.unpack_from("<I", header, 42)[0],
        "reserved": header[46:48],
    }


def build_header(payload, reserved=b"\0\0"):
    header = bytearray(HEADER_SIZE)
    header[0:32] = hashlib.sha256(payload).digest()
    header[32:32 + len(PROJECT_ID)] = PROJECT_ID
    struct.pack_into("<I", header, 42, len(payload))
    header[46:48] = reserved
    return bytes(header)


def describe_app(image):
    """Read esp_app_desc_t out of a plaintext ESP32 application image."""
    if len(image) < APP_DESC_OFFSET + 176 or image[0] != 0xE9:
        return None
    magic, = struct.unpack_from("<I", image, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        return None

    def field(start, size):
        return image[APP_DESC_OFFSET + start:APP_DESC_OFFSET + start + size].split(b"\0")[0].decode("latin1")

    return {
        "version": field(16, 32),
        "project_name": field(48, 32),
        "time": field(80, 16),
        "date": field(96, 16),
        "idf_ver": field(112, 32),
    }


def describe_signature(image):
    """Locate and validate an ESP32 Secure Boot v2 signature block.

    The block occupies the final 4 KB sector of a signed image. It is
    self-checking: a CRC32 over its own body, and a SHA-256 of everything
    preceding it.
    """
    if len(image) < 4096 or len(image) % 4096:
        return None
    offset = len(image) - 4096
    block = image[offset:offset + 1216]
    if block[0] != 0xE7 or block[1] != 0x02:
        return None

    digest = block[4:36]
    modulus = int.from_bytes(block[36:36 + 384], "little")
    exponent, = struct.unpack_from("<I", block, 36 + 384)
    signature = int.from_bytes(block[812:812 + 384], "little")
    stored_crc, = struct.unpack_from("<I", block, 1196)

    signed = image[:offset]
    encoded = pow(signature, exponent, modulus).to_bytes(384, "big")

    return {
        "offset": offset,
        "modulus_bits": modulus.bit_length(),
        "exponent": exponent,
        "crc_ok": zlib.crc32(block[:1196]) & 0xFFFFFFFF == stored_crc,
        "digest_ok": hashlib.sha256(signed).digest() == digest,
        "pss_trailer_ok": encoded[-1] == 0xBC,
        "digest": digest.hex(),
    }


def resolve_pad(args, blob):
    """Settle on a pad, and describe how sure we are of it."""
    if args.pad:
        pad = bytes.fromhex(args.pad.replace(" ", ""))
        if len(pad) != PAD_SIZE:
            sys.exit(f"--pad must be {PAD_SIZE} bytes, got {len(pad)}")
        return pad, "supplied", [], set(range(PAD_SIZE))
    if args.key:
        with open(args.key, "rb") as handle:
            return (pad_from_key(handle.read()), f"derived from {args.key}",
                    [], set(range(PAD_SIZE)))

    pad, missing, conflicts = recover_pad(blob)
    caveats = []
    for index, values in conflicts:
        if index is None:
            for text, sources in values.items():
                caveats.append("%s (%s)" % (text, ", ".join(sources)))
            continue
        caveats.append("byte %d: %s disagree"
                       % (index, " vs ".join(
                           "%s says %02x" % ("/".join(s), v)
                           for v, s in sorted(values.items()))))
    if missing:
        caveats.append("nothing in this container reveals byte%s %s"
                       % ("" if len(missing) == 1 else "s",
                          ", ".join(str(i) for i in missing)))
    established = set(range(PAD_SIZE)) - set(missing)
    for index, _ in conflicts:
        established.discard(index)
    return pad, "recovered from known plaintext", caveats, established


def cmd_inspect(args):
    blob = open(args.image, "rb").read()
    header_ct, payload_ct = split(blob)
    pad, source, caveats, established = resolve_pad(args, blob)

    header = parse_header(xor_pad(header_ct, pad, 0))
    # The web-UI path deobfuscates only the header; it writes and hashes the
    # payload as received, so a www payload is plaintext on the wire.
    is_www = blob[0] == TYPE_WWW
    payload = payload_ct if is_www else xor_pad(payload_ct, pad, HEADER_SIZE)
    digest = hashlib.sha256(payload).digest()

    print(f"file            : {args.image}")
    print(f"type            : 0x{blob[0]:02x} ({'www, payload plaintext' if is_www else 'app, payload obfuscated'})")
    print(f"size            : {len(blob)} bytes")
    shown = " ".join("%02x" % b if i in established else "??"
                     for i, b in enumerate(pad))
    print(f"pad             : {shown}  ({source})")
    for caveat in caveats:
        print(f"                  ! {caveat}")
    print(f"project id      : {header['project_id'].decode('latin1')!r}")
    print(f"declared length : {header['length']}")
    print(f"actual payload  : {len(payload)}")
    print(f"reserved [46:48]: {header['reserved'].hex(' ')}")
    print(f"embedded sha256 : {header['sha256'].hex()}")
    print(f"computed sha256 : {digest.hex()}")

    # Three outcomes, not two. A digest that does not match means the image
    # is corrupt only if the pad was right; if the pad was wrong, nothing
    # above this line decoded and the digest says nothing at all. Reporting
    # MISMATCH for both is what made this tool call a shipped release corrupt.
    #
    # The comparison runs only over the digest bytes this pad established.
    # header[n] uses pad[n % PAD_SIZE], so an unestablished pad byte garbles
    # four of the thirty-two and would otherwise sink an image that is fine.
    # Fourteen established bytes still cover twenty-eight of them, which is
    # ample to tell a payload that matches its digest from one that does not.
    problems = validate_pad(blob, pad)
    checkable = [n for n in range(32) if n % PAD_SIZE in established]
    agrees = all(digest[n] == header["sha256"][n] for n in checkable)

    if agrees and len(checkable) == 32:
        verdict = "OK"
    elif problems:
        verdict = "UNKNOWN -- this pad does not decode the header, so nothing above is meaningful"
    elif not checkable:
        verdict = "UNKNOWN -- no part of this pad is established"
    elif agrees:
        verdict = ("OK on the %d of 32 digest bytes this pad establishes; "
                   "the rest is unverified" % len(checkable))
    else:
        verdict = ("MISMATCH -- the payload does not match its digest "
                   "(judged on %d of 32 bytes)" % len(checkable))
    print(f"integrity       : {verdict}")
    for problem in problems:
        print(f"                  ! {problem}")
    if problems and source.startswith("recovered"):
        print("                  try --pad or --key; recovery could not settle this image")

    desc = describe_app(payload)
    if desc:
        print("\nesp_app_desc_t:")
        for key, value in desc.items():
            print(f"  {key:<13}: {value}")

    sig = describe_signature(payload)
    if sig:
        print("\nSecure Boot v2 signature block:")
        print(f"  offset       : 0x{sig['offset']:x}")
        print(f"  RSA          : {sig['modulus_bits']} bits, exponent {sig['exponent']}")
        print(f"  block CRC32  : {'OK' if sig['crc_ok'] else 'BAD'}")
        print(f"  image digest : {'OK' if sig['digest_ok'] else 'BAD'}")
        print(f"  PSS trailer  : {'OK' if sig['pss_trailer_ok'] else 'BAD'}")
        print("  note         : enforced only if Secure Boot eFuses are burned")
    else:
        print("\nSecure Boot v2 signature block: none found (image is unsigned)")

    return 0 if verdict.startswith("OK") else 1


def cmd_unpack(args):
    blob = open(args.image, "rb").read()
    header_ct, payload_ct = split(blob)
    pad, _, _, _ = resolve_pad(args, blob)

    header = parse_header(xor_pad(header_ct, pad, 0))
    payload = payload_ct if blob[0] == TYPE_WWW else xor_pad(payload_ct, pad, HEADER_SIZE)

    if hashlib.sha256(payload).digest() != header["sha256"] and not args.force:
        # Name which of the two it is, rather than offering the caller both
        # and letting them guess. The header check does not depend on the
        # payload, so it separates a wrong pad from a corrupt image.
        problems = validate_pad(blob, pad)
        if problems:
            sys.exit("wrong pad -- " + "; ".join(problems)
                     + "\nsupply --pad or --key (--force writes it anyway)")
        sys.exit("digest mismatch: the pad decodes the header correctly, so "
                 "this payload really does not match its digest "
                 "(--force writes it anyway)")

    payload = payload[:header["length"]]
    with open(args.output, "wb") as handle:
        handle.write(payload)
    print(f"wrote {len(payload)} bytes to {args.output}")
    return 0


def cmd_pack(args):
    payload = open(args.image, "rb").read()
    if not args.pad and not args.key:
        sys.exit("pack requires --pad or --key; there is nothing to recover a pad from")
    pad, _, _, _ = resolve_pad(args, payload)

    reserved = bytes.fromhex(args.reserved.replace(" ", ""))
    if len(reserved) != 2:
        sys.exit("--reserved must be exactly 2 bytes")

    header = build_header(payload, reserved)

    # The two update paths differ in more than the type byte. The application
    # updater deobfuscates the whole image; the web-UI updater deobfuscates the
    # header only and writes the payload exactly as received, so a www payload
    # goes out in the clear. Packing a web-UI image like an application one
    # produces something the miner accepts and then writes as garbage.
    if args.type == "www":
        blob = bytes([TYPE_WWW]) + xor_pad(header, pad, 0) + payload
    else:
        blob = bytes([TYPE_APP]) + xor_pad(header, pad, 0) + xor_pad(payload, pad, HEADER_SIZE)
    with open(args.output, "wb") as handle:
        handle.write(blob)
    print(f"wrote {len(blob)} bytes to {args.output}")
    print(f"payload sha256: {hashlib.sha256(payload).hexdigest()}")
    return 0


def cmd_recover(args):
    blob = open(args.image, "rb").read()

    if args.frequency:
        _, payload_ct = split(blob)
        pad = recover_pad_by_frequency(payload_ct, args.plain_byte)
        print(pad.hex(" "))
        print("! frequency analysis: this pad is a guess and is wrong for "
              "Stay Open images; see known_plaintext()", file=sys.stderr)
        return 1

    pad, missing, conflicts = recover_pad(blob)
    print(pad.hex(" "))

    problems = validate_pad(blob, pad) if not missing else []
    for index, values in conflicts:
        if index is None:
            for text, sources in values.items():
                print("! %s (%s)" % (text, ", ".join(sources)), file=sys.stderr)
            continue
        print("! byte %d: %s" % (index, " vs ".join(
            "%s says %02x" % ("/".join(s), v)
            for v, s in sorted(values.items()))), file=sys.stderr)
    if missing:
        print("! bytes %s are not revealed by this container; they cover "
              "header[46:48], which the firmware never reads"
              % ", ".join(str(i) for i in missing), file=sys.stderr)
    for problem in problems:
        print("! " + problem, file=sys.stderr)

    return 1 if (missing or conflicts or problems) else 0


def cmd_selftest(args):
    """Check the tool against containers whose right answers are known.

    This exists because the failure it guards against was silent. Recovery
    returned a pad with bit 3 flipped at every odd index, `inspect` decoded
    the header through it, printed a project id of 'G]LDPGWMR\x08' and a
    declared length of 137061568, and reported `integrity: MISMATCH` -- on a
    release that miners accept and install. Nothing anywhere said "I could
    not establish this pad". Pass it any containers from dist/; both an app
    and a www one are worth including, because they are established from
    different sources and only the www path reaches pad[14:16].
    """
    failures = []

    def check(what, got, want):
        if got != want:
            failures.append("%s\n    got      %r\n    expected %r" % (what, got, want))

    for path in args.images:
        label = os.path.basename(path)
        blob = open(path, "rb").read()
        header_ct, payload_ct = split(blob)
        is_www = blob[0] == TYPE_WWW

        pad, missing, conflicts = recover_pad(blob)
        check("%s: every pad byte established with no key" % label,
              (missing, [index for index, _ in conflicts]), ([], []))
        check("%s: recovered pad decodes the header" % label,
              validate_pad(blob, pad), [])

        payload = payload_ct if is_www else xor_pad(payload_ct, pad, HEADER_SIZE)
        header = parse_header(xor_pad(header_ct, pad, 0))
        check("%s: payload matches its digest under the recovered pad" % label,
              hashlib.sha256(payload).hexdigest(), header["sha256"].hex())

        # The pad the old frequency heuristic returned. It must be rejected
        # rather than believed, whatever else the tool goes on to say.
        heuristic = bytes(b ^ (0x08 if i % 2 else 0x00) for i, b in enumerate(pad))
        check("%s: the old heuristic's pad is rejected" % label,
              bool(validate_pad(blob, heuristic)), True)

        # And real damage must still be called damage.
        damaged = bytearray(blob)
        damaged[1 + HEADER_SIZE + len(payload_ct) // 2] ^= 0xFF
        damaged = bytes(damaged)
        dpad, dmissing, _ = recover_pad(damaged)
        dheader = parse_header(xor_pad(damaged[1:1 + HEADER_SIZE], dpad, 0))
        dpayload = (damaged[1 + HEADER_SIZE:] if is_www
                    else xor_pad(damaged[1 + HEADER_SIZE:], dpad, HEADER_SIZE))
        ddigest = hashlib.sha256(dpayload).digest()
        established = set(range(PAD_SIZE)) - set(dmissing)
        checkable = [n for n in range(32) if n % PAD_SIZE in established]
        check("%s: a flipped payload byte is detected" % label,
              bool(checkable) and all(ddigest[n] == dheader["sha256"][n]
                                      for n in checkable),
              False)

    for failure in failures:
        print("FAIL " + failure, file=sys.stderr)
    checks = 4 * len(args.images)
    print("%d/%d checks passed across %d image(s)"
          % (checks - len(failures), checks, len(args.images)))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def add_pad_options(p):
        p.add_argument("--pad", help=f"{PAD_SIZE}-byte pad as hex")
        p.add_argument("--key", help="file holding the 32-byte key to derive the pad from")

    p = sub.add_parser("inspect", help="show header fields and verify integrity")
    p.add_argument("image")
    add_pad_options(p)
    p.set_defaults(func=cmd_inspect)

    p = sub.add_parser("unpack", help="decrypt to a raw application image")
    p.add_argument("image")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--force", action="store_true", help="write even if the digest mismatches")
    add_pad_options(p)
    p.set_defaults(func=cmd_unpack)

    p = sub.add_parser("pack", help="build an update image from a raw application or web-UI image")
    p.add_argument("image")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--type", choices=("app", "www"), default="app",
                   help="which updater the image is for: the application "
                        "(0xAA, payload obfuscated) or the web UI (0x55, "
                        "payload plaintext). Default app.")
    p.add_argument("--reserved", default="0000",
                   help="2 bytes for header[46:48], which the firmware never reads "
                        "(default 0000; stock 2.0.3 carries 8bbb)")
    add_pad_options(p)
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("recover", help="recover the pad from the image alone")
    p.add_argument("image")
    p.add_argument("--frequency", action="store_true",
                   help="use the old frequency analysis instead of known "
                        "plaintext. Its answer cannot be checked and is wrong "
                        "for Stay Open images; it is here only for containers "
                        "carrying some other project id.")
    p.add_argument("--plain-byte", type=lambda v: int(v, 0), default=0x00,
                   help="with --frequency, the assumed commonest plaintext "
                        "byte (default 0)")
    p.set_defaults(func=cmd_recover)

    p = sub.add_parser("selftest", help="check the tool against known-good containers")
    p.add_argument("images", nargs="+")
    p.set_defaults(func=cmd_selftest)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
