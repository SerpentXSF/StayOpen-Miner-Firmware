#!/usr/bin/env python3
"""Build, release and publish every board in one step, at one version.

    python tools/ship.py bc01 bc04            # build everything, publish nothing
    python tools/ship.py bc01 bc04 --push     # ...and push the flasher
    python tools/ship.py bc01 bc04 --push --github   # ...and cut the release

--push is a publish, not a dry run: it force-pushes gh-pages, which serves
-full.bin to anyone with the URL. So it is held to the same source checks as
--github -- clean tree, commits pushed, HEAD on a remote branch -- because a
binary served from gh-pages needs its source public for exactly the reason a
release asset does.

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

# The one list of suffixes a release may carry. Imported rather than restated:
# this script had its own copy, so adding an artifact meant editing the same
# tuple in two files and the stale-artifact guard and the upload list could
# disagree about what a release contains. That disagreement is what nearly put
# a -dev container in front of owners.
from make_release import ARTIFACT_SUFFIXES

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

    Returns {tag: is_draft}.

    Asked as JSON rather than parsed out of the table: `gh release list` prints
    the title first and the tag third, so reading column zero silently returned
    a set of titles and would have let an already-published version be released
    a second time -- the exact thing this check exists to stop.

    Drafts are included and marked, because a draft holds the tag name without
    holding the tag. `gh release create` on a name a draft already claims does
    not quietly win -- it fails, after this script has done everything else --
    and a draft is also the one case where the version may already be partly
    assembled with different files. The caller says which it found, since
    "already released" and "a draft is sitting on that name" need different
    answers from the person reading the error.
    """
    out = subprocess.run(["gh", "release", "list", "--limit", "200",
                          "--json", "tagName,isDraft"],
                         cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        return None
    import json
    return {r["tagName"]: bool(r.get("isDraft"))
            for r in json.loads(out.stdout.decode(errors="replace"))}


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

    # Flags are matched by substring above, so an unrecognised one used to be
    # ignored silently. "--githb" pushed the flasher and skipped the release,
    # leaving gh-pages advertising a version with nothing behind it -- the
    # exact drift this script exists to prevent.
    known = {"--push", "--github", "--notes-file"}
    for i, a in enumerate(sys.argv[1:]):
        if a.startswith("-") and a not in known:
            if i > 0 and sys.argv[i] == "--notes-file":
                continue          # this is the path, not a flag
            sys.exit("unrecognised option %r. Known: %s"
                     % (a, ", ".join(sorted(known))))

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
        # run() executes gh with cwd=ROOT, so a path that resolves from the
        # caller's cwd could pass this check and then fail at the gh call --
        # which happens AFTER the flasher has been force-pushed.
        notes_file = os.path.abspath(notes_file)

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

    tag = "v" + version

    # Written notes are the default when they exist.
    #
    # They were opt-in through --notes-file, and the fallback is one sentence
    # pointing at the repository. So forgetting one flag would publish a
    # security release whose page said "see the repository README" -- while
    # the notes for it were written, committed, and sitting at the path
    # guessed below. Every release since 2.0.29 has had a
    # docs/releases/<version>.md, so the case needing the flag was the common
    # one.
    #
    # --notes-file still wins, and a version with no notes file still falls
    # back to the pointer rather than refusing: a release whose changes are
    # all in the log is a real case.
    if notes_file is None:
        default_notes = os.path.join(ROOT, "docs", "releases",
                                     "%s.md" % version)
        if os.path.isfile(default_notes):
            notes_file = default_notes
            print("release notes from docs/releases/%s.md" % version)

    # --push publishes binaries, so it gets the same guards as --github.
    #
    # Every check below was gated on --github alone. But publish_flasher.py
    # force-pushes gh-pages, and gh-pages serves -full.bin to anyone with the
    # URL -- no release and no checkout in between. So `ship.py bc01 bc04
    # --push` from a dirty tree published firmware whose own SOURCE.txt,
    # served from the same directory, admitted it was not built from any
    # public commit. On a GPL project that is the compliance problem these
    # checks exist to prevent; the gate just happened to name the wrong flag.
    #
    # Split rather than widened: what follows here is about the source behind
    # the bytes, which both paths publish. The tag and the asset list below
    # are about the release, and stay on --github.
    head_sha = None
    if push or github:
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
        # A dirty tree is not shippable. make_release.py notices and writes
        # "(built from a tree with uncommitted changes)" into SOURCE.txt, but
        # that is a note on an artifact nobody reads before flashing, not a
        # refusal. The version bump this script's own docstring tells you to
        # make by hand lives in sdkconfig, so forgetting to commit it is the
        # likely case, not an exotic one: the binaries would say 2.0.32 while
        # the tag names a tree saying 2.0.31.
        dirty = subprocess.run(["git", "status", "--porcelain"],
                               cwd=ROOT, capture_output=True, text=True)
        if dirty.stdout.strip():
            sys.exit("refusing to publish %s from a dirty tree -- SOURCE.txt "
                     "travels with these binaries and would name a commit "
                     "that does not describe them:\n%s"
                     % (tag, dirty.stdout.rstrip()))

        # Compare against the real remote, not a remote-tracking ref that may
        # be hours stale. Without this the check proves only that HEAD is
        # contained in whatever was last fetched.
        #
        # The return code is checked because that is the whole point of the
        # call. Ignored, a fetch that failed -- no network, an expired
        # credential, a renamed remote -- left every comparison below running
        # against whatever was last fetched, and they all passed on stale
        # refs. The guards then reported success for the one thing they exist
        # to rule out: a publish whose source is not on the remote. Failing
        # here is safe, since nothing has been published yet.
        fetched = subprocess.run(["git", "fetch", "--quiet", "origin"],
                                 cwd=ROOT, capture_output=True, text=True)
        if fetched.returncode != 0:
            sys.exit("git fetch origin failed, so nothing below can tell you "
                     "whether these binaries' source is public:\n%s\n"
                     "Fix the remote and run this again."
                     % (fetched.stderr.strip() or "(no output)"))

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
            sys.exit("refusing to publish %s with unpushed commits -- "
                     "SOURCE.txt names a commit nobody else can fetch, and "
                     "the tag would name a tree without these in it:\n%s\n"
                     "push the branch first."
                     % (tag, unpushed.stdout.rstrip()))

        # The tag is pinned to this exact commit below. `gh release create`
        # with no --target tags "the latest state of the default branch",
        # which is only the built commit by coincidence: not when shipping
        # from a release branch, and not when someone merges between the
        # fetch above and the call below. 2.0.21 shipped with its tag two
        # commits behind its binaries; this is the direction that guard
        # never covered.
        head_sha = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                  capture_output=True, text=True).stdout.strip()
        on_remote = subprocess.run(
            ["git", "branch", "--remotes", "--contains", head_sha],
            cwd=ROOT, capture_output=True, text=True)
        if not on_remote.stdout.strip():
            sys.exit("refusing to publish %s: %s is on no remote branch. On a "
                     "GPL project a published binary whose source is not "
                     "public is a compliance problem, not untidiness."
                     % (tag, head_sha[:12]))

    assets = None
    if github:
        existing = released_tags()
        if existing is None:
            sys.exit("gh is not available or not logged in; cannot cut a release")
        if tag in existing:
            if existing[tag]:
                sys.exit("%s is already claimed by a DRAFT release. `gh release "
                         "create` will refuse that name, and the draft may "
                         "already hold different files under it. Publish or "
                         "delete the draft, or bump "
                         "CONFIG_APP_PROJECT_VER." % tag)
            sys.exit("%s is already released. Bump CONFIG_APP_PROJECT_VER rather "
                     "than replacing a published release's files." % tag)

        # LICENSE and SOURCE.txt are not optional extras: the binaries are a
        # GPL-3.0 work being conveyed, so the licence travels with them and
        # SOURCE.txt names the commit they were built from.
        # Built from the names make_release.py produces rather than a *.bin
        # glob, so an unexpected file in dist/ cannot ride along. The glob
        # twice nearly shipped a stale image, and a -dev container would sort
        # ahead of the real one in the list the miner's updater searches.
        expected = ["stay-open-%s-%s-%s.bin" % (b, version, suf)
                    for b in boards for suf in ARTIFACT_SUFFIXES]
        missing = [f for f in expected
                   if not os.path.isfile(os.path.join(DIST, f))]
        if missing:
            sys.exit("dist/ is missing %d expected artifact(s): %s"
                     % (len(missing), ", ".join(missing)))
        extra = sorted(f for f in os.listdir(DIST)
                       if f.endswith(".bin") and f not in expected
                       and f != "config-dist.bin")
        if extra:
            sys.exit("dist/ holds %d .bin file(s) this release does not "
                     "produce: %s -- remove them and run this again."
                     % (len(extra), ", ".join(extra)))
        assets = sorted(expected + ["SHA256SUMS", "LICENSE", "SOURCE.txt"])
        for board in boards:
            if not any(("-%s-" % board) in a for a in assets):
                sys.exit("no %s artifacts in dist/ -- refusing to cut a release "
                         "that omits a board it claims to cover" % board)

    # Nothing leaves this machine until every check above has passed.
    #
    # The flasher used to be pushed before those checks ran, so a guard
    # doing its job still left gh-pages advertising a version with no
    # release behind it -- the same drift the checks exist to prevent, in
    # the other direction. dist/ is filled by the packaging step, so the
    # asset check has everything it needs by this point.
    #
    # The release now goes first, which is the other half of that same
    # argument. Moving the flasher after the checks stopped a *refusal* from
    # leaving gh-pages ahead of the releases page. It did nothing about a
    # `gh release create` that fails on its own -- a notes file gh rejects, an
    # upload that times out, a token without the scope -- because by then the
    # flasher had already been force-pushed. gh-pages would serve a version
    # the releases page had never heard of, with a SOURCE.txt naming a commit
    # whose tag did not exist.
    #
    # This order makes that failure the harmless way round: a release with no
    # flasher in front of it yet, fixed by re-running publish_flasher.py, and
    # nothing published that describes itself as something it is not.
    if github:
        print("\n=== github release %s ===" % tag)
        title = "Stay Open %s - %s" % (
            version, " and ".join(b.upper() for b in boards))
        if notes_file:
            notes_arg = ["--notes-file", notes_file]
        else:
            notes_arg = ["--notes",
                         "See the repository README and docs/ for what changed."]
        run(["gh", "release", "create", tag, "--target", head_sha,
             "--title", title] + notes_arg
            + [os.path.join("dist", a) for a in assets])

    print("\n=== flasher ===")
    args = [sys.executable, os.path.join("tools", "publish_flasher.py")] + boards
    if push:
        args.append("--push")
    run(args)

    if push and not github:
        print("\nFlasher pushed at %s. The GitHub release was NOT cut -- re-run "
              "with --github, or the releases page will lag the flasher." % tag)
    elif github and not push:
        # The same drift, the other way round, and it was the only one of the
        # four combinations that said nothing on its way out.
        print("\nRelease %s is cut but the flasher was NOT pushed -- re-run "
              "with --push, or the browser flasher will keep handing out the "
              "previous version." % tag)

    print("\ndone: %s, boards %s" % (version, ", ".join(boards)))


if __name__ == "__main__":
    main()
