# -*- coding: utf-8 -*-
"""Does ship.py publish a web-version marker the interface will actually read?

    python tools/test_ship_marker.py

No dependencies and no network. Exits non-zero on failure.

Why this test exists. The marker is a contract with two halves written in two
languages: ship.py appends an HTML comment to the release body, and
Settings.vue reads it back with a regex to decide whether a miner's interface
is behind the release. The halves have disagreed twice.

  1. 2.0.35's own notes described the marker in prose, inside a code span.
     ship.py tested for the marker by NAME, found the name in that sentence,
     concluded the notes already carried one, and appended nothing. The
     interface of the day took the first match in the body, so every miner was
     told a web update was available at version "...".

  2. The fix for that tested with a LOOSE pattern -- marker-shaped text
     anywhere -- while Settings.vue had moved to an ANCHORED one, where the
     marker must occupy a line of its own. So notes carrying the right version
     inside a code span satisfied ship.py's "already correct, leave it alone"
     path and shipped a release the page could not read at all. That is what
     the third case below pins down.

So this test does not hardcode the pattern. It lifts the regex literal out of
Settings.vue, compiles it in Python, and judges ship.py's output with that --
a test that agreed with itself would not have caught either failure.
"""
import io
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)        # ship.py imports make_release, its sibling
import ship                                                  # noqa: E402

VUE = os.path.join(ROOT, "main", "http_server", "axe-os", "src", "pages",
                   "Settings.vue")

WEB = "9.9.9"                   # not a real web version, on purpose
MARK = "<!-- stayopen-web-version: %s -->" % WEB

fails = []


def check(name, ok, detail=""):
    print("  %-4s %s%s" % ("PASS" if ok else "FAIL", name,
                           "" if ok else "   <- " + detail))
    if not ok:
        fails.append(name)


def interface_pattern():
    """The regex Settings.vue actually uses, compiled for Python.

    Both halves of the contract have to be read from where they live, or the
    test is only checking ship.py against a copy of ship.py.
    """
    src = io.open(VUE, encoding="utf-8").read()
    m = re.search(r"^const WEB_VERSION_RE = /(.*)/([a-z]*);\s*$", src, re.M)
    if not m:
        sys.exit("cannot find WEB_VERSION_RE in %s -- if the interface "
                 "renamed or moved it, this test has to be pointed at the "
                 "new one rather than deleted" % VUE)
    body, flags = m.group(1), m.group(2)
    if "g" not in flags:
        sys.exit("Settings.vue's WEB_VERSION_RE is no longer global; it used "
                 "matchAll and took the last match, and this test assumes it")
    return body, re.compile(body, re.M if "m" in flags else 0)


SOURCE, IFACE = interface_pattern()


def iface_reads(text):
    """What the interface would conclude the release's web version is.

    Settings.vue does `[...body.matchAll(RE)]` and then
    `marks[marks.length - 1][1].trim()`, so the LAST match wins and no match
    means the empty string, which the page reports as 'unknown'.
    """
    got = IFACE.findall(text)
    return got[-1].strip() if got else ""


print("Settings.vue declares /%s/" % SOURCE)

# --------------------------------------------------------------- the patterns
print("\n1. ship.py's anchored pattern agrees with the interface's")
SAMPLES = [
    MARK,
    "   %s   " % MARK,
    "prose `%s` prose" % MARK,
    "a\n%s\nb" % MARK,
    "<!--stayopen-web-version:%s-->" % WEB,
    "x %s" % MARK,
    "%s x" % MARK,
    "<!-- stayopen-web-version: -->",
    "",
]
for s in SAMPLES:
    want = iface_reads(s)
    got = ship.WEB_VERSION_LINE_RE.findall(s)
    got = got[-1].strip() if got else ""
    check("agree on %r" % (s[:44],), want == got,
          "interface %r, ship %r" % (want, got))

print("\n2. the loose pattern stays a superset of the anchored one")
print("   (the refusal path needs it: an interface older than 1.6.6 read the")
print("    body loosely and took the FIRST match, so prose that merely looks")
print("    like a marker can still mislead a page that has not been updated)")
for s in SAMPLES:
    loose = len(ship.WEB_VERSION_RE.findall(s))
    anch = len(ship.WEB_VERSION_LINE_RE.findall(s))
    check("superset on %r" % (s[:44],), anch <= loose,
          "anchored %d > loose %d" % (anch, loose))

# ---------------------------------------------------------- the decision logic
print("\n3. notes_with_web_version decides correctly")

real_web_version = ship.web_version
real_dist = ship.DIST
ship.web_version = lambda version, boards: WEB
tmp = tempfile.mkdtemp(prefix="ship-marker-test-")
ship.DIST = os.path.join(tmp, "dist")


def run(notes_body):
    """-> ('kept'|'appended', what the interface reads) or ('refused', message)"""
    p = os.path.join(tmp, "notes.md")
    io.open(p, "w", encoding="utf-8", newline="\n").write(notes_body)
    try:
        out, _web = ship.notes_with_web_version(p, "0.0.0-test", ["bc04"])
    except SystemExit as exc:
        return "refused", str(exc)
    published = io.open(out, encoding="utf-8").read()
    kept = os.path.abspath(out) == os.path.abspath(p)
    return ("kept" if kept else "appended"), iface_reads(published)


CASES = [
    ("no marker: append one the interface can read",
     "# notes\n\nbody.\n", "appended", WEB),
    ("correct marker on its own line: leave it alone",
     "# notes\n\nbody.\n\n%s\n" % MARK, "kept", WEB),
    ("right version but buried in prose: REFUSE (the regression)",
     "# notes\n\nship.py appends `%s` to the body.\n" % MARK, "refused", None),
    ("own line, wrong version: refuse",
     "# notes\n\n<!-- stayopen-web-version: 0.0.1 -->\n", "refused", None),
    ("two markers: refuse",
     "# notes\n\n%s\n\n%s\n" % (MARK, MARK), "refused", None),
    ("a prose copy above a real marker: refuse",
     "# notes\n\nwe write `%s`.\n\n%s\n" % (MARK, MARK), "refused", None),
    ("no trailing newline, no marker: still append",
     "# notes\n\nbody.", "appended", WEB),
]

for name, body, want_kind, want_read in CASES:
    kind, info = run(body)
    ok = kind == want_kind and (want_read is None or info == want_read)
    check(name, ok, "got %s %r, wanted %s %r"
          % (kind, (info or "")[:100], want_kind, want_read))

ship.web_version = real_web_version
ship.DIST = real_dist
shutil.rmtree(tmp, ignore_errors=True)

print()
if fails:
    print("%d check(s) FAILED: %s" % (len(fails), "; ".join(fails)))
    sys.exit(1)
print("all %d checks passed" % (len(SAMPLES) * 2 + len(CASES)))
