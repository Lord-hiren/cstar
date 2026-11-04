#!/usr/bin/env python3
"""tests/check_docs.py — verify the documentation against the real toolchain.

Two checks, both run by default:

  samples  Every ```python code block that is immediately followed by a
           ```text block (optionally with an "Output:" line between them) is
           written to `example.pen`, run with `build/pentest run`, and its
           combined output (stdout, then stderr) must equal the text block.
           Put `<!-- no-run -->` on the line before a ```python fence to skip
           a block (used for deliberately-failing "known issue" repros).

  links    Every relative Markdown link `[text](path#anchor)` must point at an
           existing file, and any `#anchor` must match a heading in it.

Usage:
  python3 tests/check_docs.py                 # all docs, both checks
  python3 tests/check_docs.py --samples       # only run the code samples
  python3 tests/check_docs.py --links         # only check links
  python3 tests/check_docs.py docs/guide/*.md # specific files

Needs build/pentest (run `make` first). Each sample compiles with clang (about
15 s for the whole tree once the runtime object is cached); --links is instant. Exit code 0 = everything passed.
Not part of `make test`; run it whenever documentation or the language changes.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PENTEST = os.path.join(ROOT, "build", "pentest")

# A match can never cross a code fence: neither group may contain ```.
SAMPLE = re.compile(
    r"(<!-- no-run -->[ \t]*\n)?```python\n((?:(?!```).)*)```[ \t]*\n+"
    r"(?:Output[^\n]*\n+)?```text\n((?:(?!```).)*)```",
    re.S,
)
LINK = re.compile(r"(?<!\!)\[[^\]]*\]\(([^)\s]+)\)")
FENCE = re.compile(r"^```")


def default_files():
    files = [os.path.join(ROOT, n) for n in ("README.md", "SPEC.md", "CHANGELOG.md")]
    for base, _dirs, names in os.walk(os.path.join(ROOT, "docs")):
        files += [os.path.join(base, n) for n in sorted(names) if n.endswith(".md")]
    files.append(os.path.join(ROOT, "editors", "vscode", "README.md"))
    return [f for f in files if os.path.exists(f)]


def rel(path):
    return os.path.relpath(path, ROOT)


def check_samples(files):
    if not os.access(PENTEST, os.X_OK):
        print("error: build/pentest not found - run `make` first", file=sys.stderr)
        return 1
    work = tempfile.mkdtemp(prefix="pentest-docs-")
    try:
        return _run_samples(files, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _run_samples(files, work):
    env = dict(os.environ, XDG_CACHE_HOME=os.path.join(work, "cache"))
    ok = failed = skipped = 0
    for md in files:
        text = open(md, encoding="utf-8").read()
        for m in SAMPLE.finditer(text):
            line = text[: m.start()].count("\n") + 1
            if m.group(1):
                skipped += 1
                continue
            src, want = m.group(2), m.group(3)
            with open(os.path.join(work, "example.pen"), "w", encoding="utf-8") as f:
                f.write(src)
            try:
                p = subprocess.run(
                    [PENTEST, "run", "example.pen"], cwd=work, env=env,
                    stdin=subprocess.DEVNULL, capture_output=True, text=True,
                    timeout=180,
                )
            except subprocess.TimeoutExpired:
                print(f"TIMEOUT  {rel(md)}:{line}")
                failed += 1
                continue
            got = p.stdout + p.stderr
            if got.rstrip("\n") == want.rstrip("\n"):
                ok += 1
            else:
                failed += 1
                print(f"MISMATCH {rel(md)}:{line}\n--- documented\n{want}--- actual\n{got}---")
    print(f"samples: {ok} passed, {failed} failed, {skipped} skipped (no-run)")
    return 1 if failed else 0


def slug(heading):
    h = heading.strip().lower().replace("`", "")
    h = re.sub(r"[^\w\- ]", "", h, flags=re.U)
    return h.replace(" ", "-")


def headings(path, cache={}):
    if path not in cache:
        slugs, in_fence = set(), False
        for ln in open(path, encoding="utf-8"):
            if FENCE.match(ln):
                in_fence = not in_fence
            elif not in_fence and ln.startswith("#"):
                slugs.add(slug(ln.lstrip("#")))
        cache[path] = slugs
    return cache[path]


def check_links(files):
    bad = n = 0
    for md in files:
        in_fence = False
        for no, ln in enumerate(open(md, encoding="utf-8"), 1):
            if FENCE.match(ln):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for target in LINK.findall(re.sub(r"`[^`]*`", "", ln)):
                if re.match(r"^[a-z]+:", target):  # http:, mailto:, ...
                    continue
                n += 1
                path, _, anchor = target.partition("#")
                dest = os.path.normpath(os.path.join(os.path.dirname(md), path)) if path else md
                if not os.path.exists(dest):
                    print(f"BROKEN   {rel(md)}:{no}: {target} (no such file)")
                    bad += 1
                elif anchor and dest.endswith(".md") and anchor not in headings(dest):
                    print(f"BROKEN   {rel(md)}:{no}: {target} (no such heading)")
                    bad += 1
    print(f"links: {n} checked, {bad} broken")
    return 1 if bad else 0


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    flags = {a for a in argv if a.startswith("--")}
    files = [os.path.abspath(a) for a in args] or default_files()
    run_all = not (flags & {"--samples", "--links"})
    rc = 0
    if run_all or "--links" in flags:
        rc |= check_links(files)
    if run_all or "--samples" in flags:
        rc |= check_samples(files)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
