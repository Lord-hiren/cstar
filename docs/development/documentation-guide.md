# Documentation guide

How the documentation is organised, which documents are authoritative, and how to
change it without letting it rot.

## Layout

```text
README.md                    landing page: what it is, install, 60-second tour, doc map
CHANGELOG.md                 per-version changes
SPEC.md                      the normative language specification (one file, stable §-numbers)
docs/
  README.md                  the index — start here
  guide/                     for people WRITING Pentest programs
  reference/                 look-up material: spec map, cross-scope access, known issues
  contracts/                 the stage contracts the SPEC derives from (normative)
  internals/                 for people changing the COMPILER or RUNTIME
  performance/               optimisation mechanisms + recorded baselines
  development/               building, testing, benchmarking, the editor, these docs
  history/                   the original design and the per-stage progress records
editors/vscode/README.md     the extension's user documentation
```

## Authority

| Tier | Documents | If it disagrees with the compiler |
|---|---|---|
| **Normative** | `SPEC.md`, `docs/contracts/*`, `docs/reference/cross-scope-access.md` | the compiler is wrong — file a bug ([known issues](../reference/known-issues.md)) or update the contract deliberately |
| **Informative** | everything else | the document is wrong — fix it |
| **Records** | `docs/history/*`, `docs/performance/baseline-*.md` | they describe a past moment; do not "update" them, add a note |

Precedence inside the normative tier: the stage **contracts govern** over the SPEC
if they differ.

## Rules that keep it correct

1. **SPEC section numbers never change.** Source comments, tests and the editor
   extension cite `SPEC §5.11.5`, `docs/reference/cross-scope-access.md §4` and
   similar. Add new material at the end of a section or as a new subsection;
   never renumber.
2. **Quoted error messages are normative.** The test suite matches on them as
   substrings. Do not reword a message in a document or in code without updating
   the tests and the SPEC together.
3. **Do not edit a contract to match new behaviour silently.** Change the
   contract first, then the code.
4. **Moving a file means updating every reference.** Code comments cite doc paths
   (`grep -rn "docs/" src runtime bench editors tests`); the editor's hover text and a
   smoke-test assertion cite one too. Use `tests/check_docs.py --links` for Markdown
   links.
5. **Counts and versions are claims.** Test counts, the version string, "N builtins"
   and "N classes" appear in several places; when they change, search for the old
   number (`grep -rn "336" README.md SPEC.md docs`). `pentest --version` is the source
   of truth for the version, `make test` for the test count.
6. **Prefer a pointer to a copy.** The language rules live in the SPEC; guides link
   to a section instead of restating it, except where a worked example helps.
7. **Mark status.** A document that describes a past moment says so at the top
   (see the provenance note in [baseline-m2c](../performance/baseline-m2c.md)).

## Executable samples

Code samples in the guides are *tested*. The convention:

* a ` ```python ` fence followed (after an optional `Output:` line) by a ` ```text `
  fence is a sample: the first is a complete program, the second is exactly what
  it prints — **standard output first, then standard error**, as one stream;
* programs are run as `example.pen` in a scratch directory, so diagnostics read
  `example.pen:LINE: …`;
* put `<!-- no-run -()` on the line before the python fence to skip a sample (used
  for repros of known bugs, whose "output" is the bug);
* a python fence *without* an adjacent text fence is illustrative and not run.

Check them with:

```bash
make                                   # the checker needs build/pentest
python3 tests/check_docs.py            # samples + links, all documents
python3 tests/check_docs.py --links    # instant: only relative links and anchors
python3 tests/check_docs.py --samples docs/guide/language-tour.md
```

The sample check compiles each program with `clang`; a full run of the current
tree takes about 15 seconds once the runtime object is cached. It is not part of `make test`; run it whenever you change the language,
a diagnostic, or a document. Anchors follow the usual Markdown slug rule
(lower-case, punctuation dropped, spaces to hyphens).

## Writing style

* Lead with what the reader wants to *do*, then the rule, then the exceptions.
* Say what is verified. If you could not run something, say so in the document.
* Use the project's vocabulary exactly: **text** (not "string"), **map** (not
  "dict"), `in` membership, "Pentest" for the language and `pentest` for the CLI.
* Give error messages verbatim, in code formatting.
* Prefer tables for reference material and short runnable examples for behaviour.

## Adding a document

1. Put it in the directory that matches its audience and authority (table above).
2. Link it from [docs/README.md](../README.md) and, if it is a guide, from the
   neighbouring guides.
3. If it is a record, state its date/version at the top.
4. Run `python3 tests/check_docs.py`.
5. Add a line to [CHANGELOG.md](../../CHANGELOG.md) under *Unreleased → Documentation*.
