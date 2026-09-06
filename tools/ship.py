#!/usr/bin/env python3
"""Build, release and publish every board in one step, at one version.

    python tools/ship.py bc01 bc04            # build everything, publish nothing
    python tools/ship.py bc01 bc04 --push     # ...and push the flasher
    python tools/ship.py bc01 bc04 --push --github   # ...and cut the release

Why this exists
---------------
Building, packaging, publishing the flasher and cutting a GitHub release were
four separate commands, and nothing checked that they agreed. They drifted:
the web flasher served 2.0.15 while the newest GitHub release was 2.0.14 and
carried BC01 files only, so a BC04 owner reading the releases page would
conclude the board was unsupported while the flasher was already handing out a
working image for it. Someone who flashed from the browser got a version that
did not exist anywhere else.

Running the steps from one place, over all the boards at once, is what keeps
them in step. The version is not passed in -- it comes from the build, so the
flasher and the release cannot describe different things.

This does not bump the version. Edit CONFIG_APP_PROJECT_VER in sdkconfig first;
publishing different content under a version that has already been released is
the thing that makes a version number worthless.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")


def run(args, **kw):
    print("  $ " + " ".join(str(a) for a in args))
    subprocess.run(args, cwd=ROOT, check=True, **kw)


def built_version(board):
    """The version the build actually produced, not one passed in."""
    sdk = os.path.join(ROOT, "build", board, "sdkconfig")
    if not os.path.exists(sdk):
        sys.exit("no build for %s -- nothing to read a version from" % board)
    with open(sdk, encoding="utf-8", errors="replace") as fh:
        m = re.search(r'^CONFIG_APP_PROJECT_VER="([^"]+)"', fh.read(), re.M)
    if not m:
        sys.exit("no CONFIG_APP_PROJECT_VER in %s" % sdk)
    return m.group(1).split()[0]


def released_tags():
    """Tags that already have a release, or None if gh cannot answer.

    Asked as JSON rather than parsed out of the table: `gh release list` prints
    the title first and the tag third, so reading column zero silently returned
    a set of titles and would have let an already-published version be released
    a second time -- the exact thing this check exists to stop.
    """
    out = subprocess.run(["gh", "release", "list", "--limit", "200",
                          "--json", "tagName"],
                         cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        return None
    import json
    return {r["tagName"] for r in json.loads(out.stdout.decode(errors="replace"))}


def main():
    argv = sys.argv[1:]
    if "--notes-file" in argv:
        i = argv.index("--notes-file")
        del argv[i:i + 2]
    boards = [a.lower() for a in argv if not a.startswith("-")]
    if not boards:
        sys.exit("usage: ship.py <board> [board...] [--push] [--github] "
                 "[--notes-file PATH]")
    push = "--push" in sys.argv
    github = "--github" in sys.argv

    # Release notes default to a pointer at the repository. That is fine for a
    # release whose changes are all in the log, and wrong for one that has
    # something an owner needs to read before flashing -- a known-broken
    # protection, a fault that needs the power pulled by hand. Those belong on
    # the releases page, not one link away from it.
    notes_file = None
    if "--notes-file" in sys.argv:
        i = sys.argv.index("--notes-file")
        if i + 1 >= len(sys.argv):
            sys.exit("--notes-file needs a path")
        notes_file = sys.argv[i + 1]
        if not os.path.isfile(notes_file):
            sys.exit("no such notes file: %s" % notes_file)

    for board in boards:
        print("\n=== building %s ===" % board)
        run([sys.executable, os.path.join("tools", "build_board.py"), board])

    # One version across every board, or the flasher would offer a choice
    # between two different releases wearing one name.
    versions = {b: built_version(b) for b in boards}
    if len(set(versions.values())) != 1:
        sys.exit("boards built at different versions: %s -- rebuild them all"
                 % ", ".join("%s %s" % kv for kv in sorted(versions.items())))
    version = next(iter(versions.values()))
    print("\nall boards at %s" % version)

    for board in boards:
        print("\n=== packaging %s ===" % board)
        run([sys.executable, os.path.join("tools", "make_release.py"), board])

    print("\n=== flasher ===")
    args = [sys.executable, os.path.join("tools", "publish_flasher.py")] + boards
    if push:
        args.append("--push")
    run(args)

    tag = "v" + version
    if github:
        """
        The source has to be public before the binaries are.

        2.0.21 shipped with its tag pointing two commits behind the code the
        images were built from, because the release was cut while those
        commits were still local. `gh release create` tags whatever the remote
        HEAD happens to be, so the tag named a tree that did not contain the
        version string in the binary it was attached to. Anyone rebuilding
        from that tag would have got different firmware -- which is the exact
        drift this script exists to prevent, in the one direction it was not
        checking.

        On a GPL project that is not a tidiness problem. A published binary
        whose source is not in the public repository is a compliance one.
        """
        branch = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            cwd=ROOT, capture_output=True, text=True).stdout.strip()

        # @{upstream} is the right question but it is not always answerable:
        # this clone tracks nothing, so it errors. Fall back to the remote
        # branch of the same name, which is what "pushed" means here.
        unpushed = subprocess.run(
            ["git", "log", "--oneline", "@{upstream}..HEAD"],
            cwd=ROOT, capture_output=True, text=True)
        if unpushed.returncode != 0:
            unpushed = subprocess.run(
                ["git", "log", "--oneline", "origin/%s..HEAD" % branch],
                cwd=ROOT, capture_output=True, text=True)
        if unpushed.returncode != 0:
            sys.exit("cannot tell whether %s is pushed (no upstream and no "
                     "origin/%s): %s"
                     % (branch, branch, unpushed.stderr.strip()))
        if unpushed.stdout.strip():
            sys.exit("refusing to cut %s with unpushed commits -- the tag "
                     "would name a tree that does not contain these:\n%s\n"
                     "push the branch first."
                     % (tag, unpushed.stdout.rstrip()))

        existing = released_tags()
        if existing is None:
            sys.exit("gh is not available or not logged in; cannot cut a release")
        if tag in existing:
            sys.exit("%s is already released. Bump CONFIG_APP_PROJECT_VER rather "
                     "than replacing a published release's files." % tag)

        assets = sorted(f for f in os.listdir(DIST)
                        if f.endswith(".bin") or f == "SHA256SUMS")
        for board in boards:
            if not any(("-%s-" % board) in a for a in assets):
                sys.exit("no %s artifacts in dist/ -- refusing to cut a release "
                         "that omits a board it claims to cover" % board)

        print("\n=== github release %s ===" % tag)
        title = "Stay Open %s - %s" % (
            version, " and ".join(b.upper() for b in boards))
        if notes_file:
            notes_arg = ["--notes-file", notes_file]
        else:
            notes_arg = ["--notes",
                         "See the repository README and docs/ for what changed."]
        run(["gh", "release", "create", tag, "--title", title] + notes_arg
            + [os.path.join("dist", a) for a in assets])
    elif push:
        print("\nFlasher pushed at %s. The GitHub release was NOT cut -- re-run "
              "with --github, or the releases page will lag the flasher." % tag)

    print("\ndone: %s, boards %s" % (version, ", ".join(boards)))


if __name__ == "__main__":
    main()
