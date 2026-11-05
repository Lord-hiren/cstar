# Editor extension (VSCode)

`editors/vscode/` is a dependency-free VSCode extension for cstar source files
(`.cst`, plus the legacy `.pen` / `.pent` until v0.4). Users read its
[own README](../../editors/vscode/README.md) (features, settings, install).
This page is for people changing it.

## What it provides

Syntax highlighting (TextMate grammar), completion and hover (driven by tables in
the extension source), snippets, live diagnostics from `cstar check`, a Run
button, comment toggling, auto-indent and auto-pairing. It never compiles or
executes code except through the explicit **Run** command.

## Layout

| File | Role | Size |
|---|---|---|
| `package.json` | manifest: id `cstar-lang.cstar-lang`, language id `cstar` for `.cst`/`.pen`/`.pent`, the `cstar.run` command and menus, settings (`cstar.executable`, `cstar.diagnostics`, `cstar.diagnosticsDelay`, plus the deprecated `pentest.*` three), editor defaults for `[cstar]` (4-space indent, tab completion) | version `0.2.1` |
| `src/extension.js` | activation, completion, hover, diagnostics runner, Run terminal, comment toggle | ~1,500 lines |
| `syntaxes/cstar.tmLanguage.json` | the TextMate grammar (`source.cstar`; every scope ends in `.cstar`) | ~200 lines |
| `snippets/cstar.json` | snippets | 27 prefixes |
| `language-configuration.json` | comments, brackets, auto-closing pairs, indentation rules | — |
| `test/smoke.js` | the smoke test | ~2,230 lines, 1363 checks |

### The tables inside `src/extension.js`

Completion and hover read from a handful of constant tables. When the language
changes, these are the places that must follow:

| Table | Holds |
|---|---|
| `RESERVED` | the reserved builtin names (completion suppresses them as new identifiers) |
| `KEYWORD_DOCS` | keyword hover/completion text |
| `EXCEPTION_DOCS` | the exception classes |
| `VARQUAL_QUALIFIER_DOCS` | the `variable.` qualifiers |
| `BUILTIN_DOCS` | one entry per builtin: signature and contract text |
| `METHOD_DOCS` | per-type method documentation shown after a `.` |
| `LEGACY_HOVER_DOCS` | hover text for `print`/`range` and constants |

`RESERVED` must contain every builtin name the compiler reserves (78: the names
in `is_reserved()` in `src/semantic.cpp` plus the tables in `src/builtins.h`).

## Keeping it in sync with the language

For each language change, check the matching extension surface:

| Change | Update |
|---|---|
| new keyword | grammar keyword scopes, `KEYWORD_DOCS`, `RESERVED` if applicable, `language-configuration.json` indentation rules (if it opens a block), a snippet |
| declaration or call syntax change | grammar scopes, `functionDeclarations()` (the line scanner behind hover and identifier/`variable.` completions), completion snippets and hover examples |
| new builtin | `BUILTIN_DOCS` (signature + contract + SPEC pointer), `RESERVED`, the grammar's builtin-call scope, optionally a snippet |
| new method on a builtin type | `METHOD_DOCS` |
| new exception class | `EXCEPTION_DOCS`, grammar |
| renamed or moved documentation | the doc paths cited in hover text and snippet descriptions |

The smoke test asserts on these lists, so a forgotten update usually fails a
check rather than going unnoticed.

## Settings and the legacy names

Settings are read through one helper, `setting(key, default)`: a `cstar.*`
setting wins when the user has set it (at any level, checked with
`inspect()`); otherwise the matching legacy `pentest.*` setting is used if
set; otherwise the default. The `pentest.*` settings stay in `package.json`
with a `deprecationMessage`, and the commands are registered under both
prefixes (`cstar.run` / `pentest.run`, `cstar.toggleLineComment` /
`pentest.toggleLineComment`), so existing settings and keybindings keep
working. All legacy names go away in v0.4, together with the `.pen` / `.pent`
associations.

## How diagnostics work

The extension runs `cstar check <file>` when a document is opened, after edits
(debounced by `cstar.diagnosticsDelay`, default 400 ms), and immediately on
save, then parses stderr lines of the form `file:line:col: error: message` into
VSCode diagnostics. The check runs on a temporary `cstar-check-….cst` copy, with
the file's folder added to `CSTAR_PATH` (and to `PENTEST_PATH`, for older CLIs)
so its imports still resolve.

The CLI is located as follows: if `cstar.executable` contains a `/` it is used
as-is; otherwise the candidates are tried in order — the bare name on `PATH`,
`~/.local/bin/<name>`, and a **hard-coded** `~/Projects/Pentest/build/<name>`
(a developer-machine convenience; clone elsewhere and set
`cstar.executable`, or symlink into `~/.local/bin`). While the setting is at
its default, the same three places are then tried for the legacy `pentest`
name. If none works the user is warned once and highlighting, snippets and
completion keep working.

The toolchain is Linux-only until v0.4, so on Windows that warning is the
expected state: everything except diagnostics and Run works natively, and the
smoke test runs there too.

## How Run works

The ▶ button, the context menu and the palette command save the file first, then
send `cstar run <file>` to a reusable **cstar** terminal (a real terminal, so
`input()` prompts work). The file run is the one the button was clicked for, not
merely the active editor.

## Testing

```bash
node editors/vscode/test/smoke.js      # also: cd editors/vscode && npm test
```

No network and no dependencies: it loads the grammar, snippets and manifest,
tokenises sample lines using the grammar's real regular expressions, and drives
the completion and hover providers against a stubbed `vscode` API. Exit code `0`
means every check passed (currently 1363).

## Installing a development copy

```bash
./install-vscode-extension.sh          # from the repo root
# then: Ctrl+Shift+P → "Developer: Reload Window"
```

The script links `editors/vscode` into the extensions folder as
`cstar-lang.cstar-lang` (a directory junction on Windows, because Git Bash's
`ln -s` would silently copy). It first removes any other link in that folder
that resolves to this `editors/vscode`, such as the pre-rename
`pentest-lang.pentest-lang`, and only reports (never deletes) real folders with
a legacy name. To uninstall, delete the `cstar-lang.cstar-lang` link.
