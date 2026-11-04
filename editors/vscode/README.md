# Pentest Language — VSCode extension

Syntax highlighting, Tab-selectable autocomplete, snippets, and live error
squiggles for Pentest source files (`.pen` / `.pent`).

## Features

| Feature | How it works |
|---|---|
| **Syntax coloring** | TextMate grammar: comments (all four forms incl. `//`), strings + escapes, **bytes literals `b"..."` (with `\xHH`)** — a bare identifier ending in `b` (e.g. `bad`) is never a bytes literal —, numbers, M1 keywords, the Stage 2A control-flow keywords (`if`/`elif`/`else`/`for`/`while`/`switch`/`case`/`break`/`continue`/`and`/`or`/`not`/`pass`), the Stage 2B declaration keywords **`global`/`nonlocal`** (`storage.modifier`), the Stage 2C keywords **`try`/`except`/`finally`/`raise`/`as` (exceptions) and `class`/`is` (objects + identity)**, operators (`()`, `**`, `&`/`\|`/`^`/`~`/`<<`/`>>`, all eleven compound assignments `+= -= *= /= %= **= &= \|= ^= <<= >>=`), **collection literals `[] {} () {"k": v}` with dots for method calls and attribute reads (`xs.append`, `p.x`)**, **slice colons (`a[1:2]`, `a[:2]`, `a[1:]`, `a[1:2:3]` get their own scope — statement/`case`/map colons untouched)**, `name():` / `name(params):` / `main` / `class` definition names, assignment targets (incl. `xs[i] = v` and dotted `p.x = v`), calls, the builtins `print range len str int float bool type encode decode set collect input time clock` **plus the 0.2.1 Python-parity set — `abs all any ascii bin bytes callable chr compile delattr dict dir divmod enumerate eval exec filter format getattr globals hasattr hash hex id isinstance iter list locals map max min next oct open ord pow repr reversed round setattr slice sorted sum tuple vars zip` and the nine stubs `breakpoint classmethod help issubclass memoryview object property staticmethod super` (every name matched before `()`, SPEC §5.11)**, **the 0.2.1 cross-scope construct `variable.<qualifier> <name>`** (`variable` → `keyword.other`, the dot → accessor punctuation, the qualifier — `global`/`parent`/a function name — → its own variable scope; a bare `variable` identifier is untouched) |
| **Autocomplete** | Every keyword (M1 + Stage 2A + **2B `global`/`nonlocal`** + **2C `try except finally raise as class is`**, incl. `()` (the call operator) and `in` (both membership and `for`-binding examples)), builtins `print` and `range` plus the reserved call-only builtins **2B `len str int float bool type encode decode set` + 2C `collect` + post-2C `input time clock`** + the **0.2.1 Python-parity set — all 43 `abs … zip` entries, the nine always-fail stubs (`super`, `property`, …) and the compile-time-rejected `eval`/`exec`/`compile`** (snippet inserts + contract docs each, including the documented deviations — `dir()` lists globals, `divmod` truncates, `slice()` returns a tuple; SPEC §5.11) — `open()`'s docs enumerate the file methods (`readline`, `readlines`, `writelines`, `seek`, `tell`, `flush`, `close`) (snippet inserts + contract docs; `type()` docs list all 14 type display names — **`text`**, not `string`, plus 2C `class`/`method` and instance-as-class-name), **every 2B method name after a `.` (`list` append/insert/index/pop/remove/reverse/clear, `map` get/keys/values/items/pop/clear, `set` add/remove/discard/pop/clear, `text` upper/lower/split/join/find/replace/strip/starts_with/ends_with, `bytes` decode — the detail names the owning types)** (user-class methods are dynamic — discoverable from the class itself), plus every function (`name():` / `name(params):`) and variable declared in the open file — accept with **Tab** or Enter. **0.2.1 cross-scope (`variable.`) surface**: typing `variable.` offers exactly the qualifiers — **`variable.global`, `variable.parent`, and every function declared in the file** (never methods), each with its semantics and scope rule — and a contextual **`variable`** item whose snippet inserts `variable.${1:global} ${2:name}` |
| **Hover** | Pointer-over docs from the same tables as autocomplete: every builtin's **signature (placeholders stripped) + full contract** with its SPEC pointer, keywords and constants, the twelve builtin exception classes (when Pentest raises them, exact-class catching, `.type`/`.message`), method names right of a dot (per-type contracts), the `variable.` qualifier form, and user function (`name():` / `name(params):`) and `class` declarations (declaration signature + line). |
| **Snippets** | `function`, `main`, `print`, `return`, plus the Stage 2A shapes: `if`, `ifelse`, `ifin` (membership `()` test), `while`, `for` (over `range(...)`), `foreach` (over a collection), `switch` (case + `case *:`), `pass` — plus the Stage 2B shapes: `functionargs` (`*args`/`**kwargs`), `functionnested` (nested closure), `global`, `nonlocal` — plus the Stage 2C shapes: `class`, `init` (`__init__`), `try` (try/except/`as`), `raise` — plus the 0.2.1 shape `variable` (inserts `variable.<qualifier> <name>`; the qualifier is `global` — the **module global** without a `global` decl, `parent` — the **enclosing function's** binding without `nonlocal`, or `variable.<fn>` — a **sibling/self function's** binding sharing this scope; see `docs/reference/cross-scope-access.md`) — plus the 0.2.1 Python-parity shapes `open` (open a file, iterate its lines, close it), `sorted` (sorted list with `key=`) and `isinstance` (exact-type `if` test) |
| **Live error squiggles** | Runs `pentest check` (parse + semantic analysis only — **never compiles or executes code**) debounced while you type; errors appear as red underlines with exact `line:col` messages |
| **Run button** | **▶ in the editor title bar** (and the editor right-click menu, plus Command Palette → "Pentest: Run File") runs **the file you are on**: the editor the ▶ was clicked for (VSCode passes it as the command resource), falling back to the active document for palette/keybindings — saved first if dirty, so stale code never runs — via `pentest run <file>` in a reusable **Pentest** terminal (real terminal, so `input()` prompts are answerable; closed terminals are recreated, so the button keeps working). The CLI path follows the `pentest.executable` setting (`~/.local/bin`, then `~/Projects/Pentest/build` fallback) |
| **Editor niceties** | `#` line / `/* */` block comment toggling (Ctrl+/, `#` style) plus the `pentest.toggleLineComment` command which toggles **both `#` and `//`** (bind it via `keybindings.json`), auto-indent after `if`/`elif`/`else`/`for`/`while`/`switch`/`case`/`class`/`try`/`except`/`finally`/`main` headers and function declarations (`name():` / `name(params):`) with `else:`/`elif:`/`case`/`except:`/`finally:` dedent-back, auto-pairs for `()`, `[]`, `{}` and quotes |

## Install

From the repository:

```bash
ln -s "$(pwd)/editors/vscode" ~/.vscode/extensions/pentest.pentest-lang-0.2.1
# then restart VSCode (or: Ctrl+Shift+P → "Developer: Reload Window")
```

Open any `.pen` or `.pent` file — the status bar shows **Pentest**.

To make Ctrl+/ toggle both `#` and `//` comments, add to `keybindings.json`:

```json
{ "key": "ctrl+/", "command": "pentest.toggleLineComment",
  "when": "editorTextFocus && editorLangId == pentest" }
```

## Smoke test

```bash
node editors/vscode/test/smoke.js   # from the repo root (npm test also works here)
```

No network, no dependencies: it loads the grammar/snippets/package files,
tokenizes sample lines with the real TextMate regexes (bytes literals, slice
colons, keywords…), and invokes the completion provider against a stubbed
`vscode` API (keywords, 2B builtins, reserved-name suppression, method
completions after `.`, and function declarations with the block syntax). Exit code 0 means every check passed.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `pentest.executable` | `pentest` | CLI used for diagnostics. The default searches `PATH`, `~/.local/bin`, then the dev build |
| `pentest.diagnostics` | `true` | Toggle live error squiggles |
| `pentest.diagnosticsDelay` | `400` | Debounce in ms before re-checking |

> **New in this tree:** `http_post(url, body)` (libcurl-backed HTTP POST) is
> now highlighted and auto-suggested as a builtin — see
> [docs/guide/builtins.md](../../docs/guide/builtins.md). A ready-to-use Jev
> decision client ships in `extras/AI.pen` (`from AI import jev` → `jev.noul`,
> `jev.choose`, `jev.score`).

Diagnostics need the `pentest` CLI (`make` in the repo root, installed to
`~/.local/bin/pentest`). If it cannot be found, a warning is shown once and
highlighting/snippets keep working without errors.

The CLI is looked up as: a path given in `pentest.executable` (if it contains
`/`), else `pentest` on `PATH`, then `~/.local/bin/pentest`, then a hard-coded
`~/Projects/Pentest/build/pentest` developer fallback.

## For maintainers

How the extension is structured, which tables must follow language changes, and
how its smoke test works: [docs/development/editor-extension.md](../../docs/development/editor-extension.md).
The language itself is documented from [docs/README.md](../../docs/README.md).
