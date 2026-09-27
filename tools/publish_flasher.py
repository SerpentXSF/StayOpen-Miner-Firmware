"""Publish the web flasher to the gh-pages branch.

    python tools/publish_flasher.py bc01              # build the branch
    python tools/publish_flasher.py bc01 bc04 --push  # both boards, pushed
    python tools/publish_flasher.py bc01 --version 2.0.27.1 --push

The version is not normally passed in: it is read from the sdkconfig this tree
would build, so the flasher cannot advertise something the source does not
describe. dist/ accumulates every version ever packaged, and picking "whatever
is in there" either breaks when there is more than one or silently publishes
the wrong one. --version overrides it for the case where you are deliberately
republishing an older release, and says so when you do.

Every board named is published together, each with its own image and its own
manifest-<board>.json, because the page offers a choice between them and a
branch carrying only one of the pair is a flash that cannot work.

The flasher has to serve the firmware image from the same origin as the page:
esp-web-tools fetches it with XHR, and GitHub release assets do not send CORS
headers, so a release URL cannot be used. That means a 12 MB binary has to live
somewhere Pages can serve.

Keeping it on main put a new 12 MB blob in the project's permanent history at
every release. Here each publish writes gh-pages as a single **orphan** commit
with no parent, so the previous image is not referenced by anything afterwards
and the branch never accumulates. gh-pages is generated output, not source --
it is meant to be replaced wholesale, which is why the push is a force.

The branch is built with git plumbing rather than by checking anything out, so
this never touches the working tree, the index, or the current branch.
"""
import os
import subprocess
import json
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "docs", "flasher")
DIST = os.path.join(ROOT, "dist")
BRANCH = "gh-pages"

REDIRECT = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>SerpentX | Stay Open</title>
<meta http-equiv="refresh" content="0; url=flasher/">
<link rel="canonical" href="flasher/">
</head>
<body>
<p>Go to the <a href="flasher/">firmware flasher</a>.</p>
</body>
</html>
"""


def git(*args, data=None):
    out = subprocess.run(["git"] + list(args), cwd=ROOT, input=data,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        sys.exit("git %s failed: %s" % (" ".join(args),
                                        out.stderr.decode(errors="replace")))
    return out.stdout.decode().strip()


def blob(path):
    """Write a file into the object store and return its sha."""
    return git("hash-object", "-w", "--", path)


def blob_bytes(data, name):
    """Write bytes into the object store via a temp file, return its sha."""
    tmp = os.path.join(DIST, ".publish-tmp-" + name)
    os.makedirs(DIST, exist_ok=True)
    with open(tmp, "wb") as fh:
        fh.write(data)
    try:
        return blob(tmp)
    finally:
        os.remove(tmp)


def mktree(entries):
    """entries: list of (mode, type, sha, name)."""
    spec = "".join("%s %s %s\t%s\n" % e for e in entries)
    return git("mktree", data=spec.encode())


def project_version():
    """The version this tree builds, from sdkconfig.

    Same rule ship.py states: the version comes from the build rather than
    from an argument, so the flasher and the release cannot end up describing
    different things.
    """
    path = os.path.join(ROOT, "sdkconfig")
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("CONFIG_APP_PROJECT_VER="):
                    raw = line.split("=", 1)[1].strip().strip('"')
                    return raw.split()[0]      # "2.0.28 20260922" -> "2.0.28"
    except OSError as exc:
        sys.exit("could not read %s: %s" % (path, exc))
    sys.exit("no CONFIG_APP_PROJECT_VER in sdkconfig")


def check_manifest(path, board, image):
    """A manifest that disagrees with its image is worse than no manifest.

    esp-web-tools fetches whatever parts[].path names. If that is a file this
    publish is not shipping, the flash fails at the download; if it names a
    different version than the manifest claims, the page tells the owner one
    thing and writes another. Neither is visible from the page itself, so it
    is checked here.
    """
    try:
        with open(path, encoding="utf-8") as fh:
            m = json.load(fh)
    except (OSError, ValueError) as exc:
        sys.exit("dist/manifest-%s.json is unreadable: %s" % (board, exc))

    try:
        named = m["builds"][0]["parts"][0]["path"]
    except (KeyError, IndexError, TypeError):
        sys.exit("dist/manifest-%s.json has no builds[0].parts[0].path" % board)

    if named != image:
        sys.exit("dist/manifest-%s.json points at %r but this publish carries "
                 "%r -- re-run tools/make_release.py %s"
                 % (board, named, image, board))

    stated = m.get("version")
    if stated and stated not in image:
        sys.exit("dist/manifest-%s.json says version %r, which is not the "
                 "version in %r -- re-run tools/make_release.py %s"
                 % (board, stated, image, board))


def main():
    argv = sys.argv[1:]
    boards = [a.lower() for a in argv if not a.startswith("-")] or ["bc01"]
    push = "--push" in argv

    version = None
    if "--version" in argv:
        i = argv.index("--version")
        if i + 1 >= len(argv):
            sys.exit("--version needs a value, e.g. --version 2.0.27.1")
        version = argv[i + 1]
        boards = [b for b in boards if b != version]
        print("publishing %s because --version was given; the source tree "
              "builds %s" % (version, project_version()))
    else:
        version = project_version()

    if not os.path.isdir(SRC):
        sys.exit("no %s to publish" % SRC)

    # Images are taken from dist/, which make_release.py fills, so the branch
    # always carries artifacts that were actually built for a release rather
    # than whatever happens to be lying in docs/.
    #
    # Every board named has to be complete before anything is published. A
    # branch carrying one board's image and another board's manifest is worse
    # than not publishing: the page offers a flash that cannot work.
    images = {}
    for board in boards:
        image = "stay-open-%s-%s-full.bin" % (board, version)
        if not os.path.exists(os.path.join(DIST, image)):
            have = []
            if os.path.isdir(DIST):
                have = sorted(f for f in os.listdir(DIST)
                              if f.startswith("stay-open-%s-" % board)
                              and f.endswith("-full.bin"))
            msg = "dist/%s is missing -- run tools/make_release.py %s first." % (image, board)
            if have:
                msg += "  dist/ currently has: " + ", ".join(have)
            sys.exit(msg)
        manifest = os.path.join(DIST, "manifest-%s.json" % board)
        if not os.path.exists(manifest):
            sys.exit("dist/manifest-%s.json is missing -- run "
                     "tools/make_release.py %s first" % (board, board))
        check_manifest(manifest, board, image)
        images[board] = (image, manifest)

    flasher = []
    for name in sorted(os.listdir(SRC)):
        full = os.path.join(SRC, name)
        if os.path.isfile(full) and not name.endswith(".bin") \
                and not name.startswith("manifest"):
            flasher.append(("100644", "blob", blob(full), name))
    for board in boards:
        image, manifest = images[board]
        flasher.append(("100644", "blob", blob(os.path.join(DIST, image)), image))
        flasher.append(("100644", "blob", blob(manifest),
                        "manifest-%s.json" % board))
    flasher.sort(key=lambda e: e[3])

    root = [
        ("100644", "blob", blob_bytes(b"", "nojekyll"), ".nojekyll"),
        ("100644", "blob", blob_bytes(REDIRECT.encode(), "index"), "index.html"),
        ("040000", "tree", mktree(flasher), "flasher"),
    ]

    tree = mktree(root)
    commit = git("commit-tree", tree, "-m",
                 "Publish flasher for %s"
                 % ", ".join("%s (%s)" % (b, images[b][0]) for b in boards))
    git("update-ref", "refs/heads/" + BRANCH, commit)

    print("built %s as a single orphan commit %s" % (BRANCH, commit[:12]))
    for mode, _, _, name in root:
        print("   ", name + ("/" if mode == "040000" else ""))
    for _, _, _, name in flasher:
        print("      flasher/" + name)

    if push:
        # Force: the branch is regenerated wholesale every time, and replacing
        # it is what stops old images being referenced.
        git("push", "--force", "origin", BRANCH)
        print("\npushed %s" % BRANCH)
    else:
        print("\nnot pushed. re-run with --push")


if __name__ == "__main__":
    main()
