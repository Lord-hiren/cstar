# Editor extension (VSCode)

`editors/vscode/` is a dependency-free VSCode extension for `.pen` / `.pent`
files. Users read its [own README](../../editors/vscode/README.md) (features,
settings, install). This page is for people changing it.

## What it provides

Syntax highlighting (TextMate grammar), completion and hover (driven by tables in
the extension source), snippets, live diagnostics from `pentest check`, a Run
button, comment toggling, auto-indent and auto-pairing. It never compiles or
executes code except through the explicit **Run** command.

## Layout

| File | Role | Size |
|---|---|---|
| `package.json` | manifest: language id `pentest`, extensions `.pen`/`.pent`, the `pentest.run` command and menus, settings (`pentest.executable`, `pentest.diagnostics`, `pentest.diagnosticsDelay`), editor defaults for `[pentest]` (4-space indent, tab completion) | version `0.2.1` |
| `src/extension.js` | activation, completion, hover, diagnostics runner, Run terminal, comment toggle | ~1,300 lines |
| `syntaxes/pentest.tmLanguage.json` | the TextMate grammar | ~190 lines |
| `snippets/pentest.json` | snippets | 24 prefixes |
| `language-configuration.json` | comments, brackets, auto-closing pairs, indentation rules | — |
| `test/smoke.js` | the smoke test | ~1,990 lines, 1241 checks |

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

## How diagnostics work

The extension runs `pentest check <file>` when a document is opened, after edits
(debounced by `pentest.diagnosticsDelay`, default 400 ms), and immediately on
save, then parses stderr lines of the form `file:line:col: error: message` into
VSCode diagnostics.

The CLI is located as follows: if `pentest.executable` contains a `/` it is used
as-is; otherwise the candidates are tried in order — the bare name on `PATH`,
`~/.local/bin/<name>`, and a **hard-coded** `~/Projects/Pentest/build/<name>`
(a developer-machine convenience; clone elsewhere and set
`pentest.executable`, or symlink into `~/.local/bin`). If none works the user is
warned once and highlighting, snippets and completion keep working.

## How Run works

The ▶ button, the context menu and the palette command save the file first, then
send `pentest run <file>` to a reusable **Pentest** terminal (a real terminal, so
`input()` prompts work). The file run is the one the button was clicked for, not
merely the active editor.

## Testing

```bash
node editors/vscode/test/smoke.js      # also: cd editors/vscode && npm test
```

No network and no dependencies: it loads the grammar, snippets and manifest,
tokenises sample lines using the grammar's real regular expressions, and drives
the completion and hover providers against a stubbed `vscode` API. Exit code `0`
means every check passed (currently 1241).

## Installing a development copy

```bash
ln -s "$(pwd)/editors/vscode" ~/.vscode/extensions/pentest.pentest-lang-0.2.1
# then: Ctrl+Shift+P → "Developer: Reload Window"
```
