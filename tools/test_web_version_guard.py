# -*- coding: utf-8 -*-
"""Does ship.py's WEB_VERSION guard catch an interface change that did not
bump the version?

Run it with `python tools/test_web_version_guard.py`. No dependencies, no
network, nothing built, nothing written. Exits non-zero on failure.

The cases are real commits from this repository's history, which is the point:
the guard exists because of 4961ee7 and a785a19, two interface changes that
were committed and pushed with WEB_VERSION untouched, and a test built from
invented fixtures would not demonstrate that it catches the thing it was
written for. On a shallow clone those commits are absent and each missing case
is skipped with a note rather than failing -- a skip is honest, a pass would
not be.

If the guard is ever deleted as redundant, this file is the argument against
it: every refusal below is a release that would have told every miner its
interface was unchanged when it was not.
"""
import importlib.util
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

spec = importlib.util.spec_from_file_location(
    "ship", os.path.join(ROOT, "tools", "ship.py"))
ship = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ship)

if not hasattr(ship, "check_web_version_bumped"):
    sys.exit("ship.py has no check_web_version_bumped. If the guard was "
             "renamed, point this test at the new name; if it was removed, "
             "read the docstring above before agreeing that it should have "
             "been.")


def exists(rev):
    return subprocess.run(["git", "rev-parse", "--verify", "--quiet",
                           rev + "^{commit}"],
                          cwd=ROOT, capture_output=True).returncode == 0


def ask(rev):
    """None if the guard passed the revision, else its refusal text.

    One wart on the historical path: the refusal quotes the version declared
    in the working tree, so it names today's value rather than the one that
    commit carried. Harmless where it matters, since in a real release the
    tree is the subject.
    """
    try:
        ship.check_web_version_bumped(rev)
    except SystemExit as exc:
        return str(exc)
    return None


CASES = [
    ("4961ee7", True,
     "the pool-down alert: two interface commits past the last bump"),
    ("a785a19", True,
     "the update row's unknown state: one interface commit past the bump"),
    ("7ded534", True,
     "the failover indicator, which also landed without a bump at the time"),
    ("d5a31f8", False,
     "the bump commit itself -- change and bump together, the normal case"),
    ("ee8c5a5", False,
     "nine docs and tooling commits since the bump, no interface change: the "
     "guard is about the interface, not about activity"),
    ("v2.0.35", False,
     "a released tag must pass, or the guard is calling history a defect"),
]

bad = skipped = 0
for rev, must_refuse, why in CASES:
    if not exists(rev):
        skipped += 1
        print("  SKIP  %-10s not in this clone -- %s" % (rev, why))
        continue
    refused = ask(rev) is not None
    ok = refused == must_refuse
    bad += 0 if ok else 1
    print("  %s  %-10s %-8s %s" % ("PASS" if ok else "FAIL", rev,
                                   "refused" if refused else "passed", why))

# The guard must also refuse to build a tree no commit describes, because a
# published binary whose source is not public is a compliance problem. Asked
# of the working tree, so its answer depends on what is checked out -- which
# makes it reportable rather than assertable.
live = ask(None)
print("\n  working tree: %s" % ("refused -- %s" % live.splitlines()[0]
                                if live else "would build"))

print()
if bad:
    sys.exit("%d FAILED" % bad)
print("all %d checks passed%s"
      % (len(CASES) - skipped, " (%d skipped)" % skipped if skipped else ""))
