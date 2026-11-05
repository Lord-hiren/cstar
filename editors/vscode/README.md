# cstar Language — VSCode extension

Syntax highlighting, Tab-selectable autocomplete, snippets, and live error
squiggles for cstar source files (`.cst`; the legacy `.pen` / `.pent` are
still recognised until v0.4).

## Features

| Feature | How it works |
|---|---|
| **Syntax coloring** | TextMate grammar: comments (all four forms incl. `//`), strings + escapes, **bytes literals `b"..."` (with `\xHH`)** — a bare identifier ending in `b` (e.g. `bad`) is never a bytes literal —, numbers, M1 keywords, the Stage 2A control-flow keywords (`if`/`elif`/`else`/`for`/`while`/`switch`/`case`/`break`/`continue`/`and`/`or`/`not`/`pass`), the Stage 2B declaration keywords **`global`/`nonlocal`** (`storage.modifier`), the Stage 2C keywords **`try`/`except`/`finally`/`raise`/`as` (exceptions) and `class`/`is` (objects + identity)**, operators (`()`, `**`, `&`/`\|`/`^`/`~`/`<<`/`>>`, all eleven compound assignments `+= -= *= /= %= **= &= \|= ^= <<= >>=`), **collection literals `[] {} () {"k": v}` with dots for method calls and attribute reads (`xs.append`, `p.x`)**, **slice colons (`a[1:2]`, `a[:2]`, `a[1:]`, `a[1:2:3]` get their own scope — statement/`case`/map colons untouched)**, `name():` / `name(params):` / `main` / `class` definition names, assignment targets (incl. `xs[i] = v` and dotted `p.x = v`), calls, the builtins `print range len str int float bool type encode decode set collect input time clock`, the HTTP builtins `http_get http_post url_encode`, **the SIMD vector builtins `fvec ivec fvec_fill ivec_fill dot`** **plus the 0.2.1 Python-parity set — `abs all any ascii bin bytes callable chr compile delattr dict dir divmod enumerate eval exec filter format getattr globals hasattr hash hex id isinstance iter list locals map max min next oct open ord pow repr reversed round setattr slice sorted sum tuple vars zip` and the nine stubs `breakpoint classmethod help issubclass memoryview object property staticmethod super` (every name matched before `()`, SPEC §5.11)** — a method call such as `v.dot(w)` stays a method call —, **the 0.2.1 cross-scope construct `variable.<qualifier> <name>`** (`variable` → `keyword.other`, the dot → accessor punctuation, the qualifier — `global`/`parent`/a function name — → its own variable scope; a bare `variable` identifier is untouched) |
| **Autocomplete** | Every keyword (M1 + Stage 2A + **2B `global`/`nonlocal`** + **2C `try except finally raise as class is`**, incl. `()` (the call operator) and `in` (both membership and `for`-binding examples)), builtins `print` and `range` plus the reserved call-only builtins **2B `len str int float bool type encode decode set` + 2C `collect` + post-2C `input time clock`** + the HTTP builtins `http_get http_post url_encode` + **the SIMD vector builtins `fvec ivec fvec_fill ivec_fill dot`** (float64 / int64 lanes, element-type errors, `dot` is the inner product) + the **0.2.1 Python-parity set — all 43 `abs … zip` entries, the nine always-fail stubs (`super`, `property`, …) and the compile-time-rejected `eval`/`exec`/`compile`** (snippet inserts + contract docs each, including the documented deviations — `dir()` lists globals, `divmod` truncates, `slice()` returns a tuple; SPEC §5.11) — `open()`'s docs enumerate the file methods (`readline`, `readlines`, `writelines`, `seek`, `tell`, `flush`, `close`) (snippet inserts + contract docs; `type()` docs list all 14 type display names — **`text`**, not `string`, plus 2C `class`/`method` and instance-as-class-name), **every 2B method name after a `.` (`list` append/insert/index/pop/remove/reverse/clear, `map` get/keys/values/items/pop/clear, `set` add/remove/discard/pop/clear, `text` upper/lower/split/join/find/replace/strip/starts_with/ends_with, `bytes` decode — the detail names the owning types)** (user-class methods are dynamic — discoverable from the class itself), plus every function (`name():` / `name(params):`) and variable declared in the open file — accept with **Tab** or Enter. **0.2.1 cross-scope (`variable.`) surface**: typing `variable.` offers exactly the qualifiers — **`variable.global`, `variable.parent`, and every function declared in the file** (never methods), each with its semantics and scope rule — and a contextual **`variable`** item whose snippet inserts `variable.${1:global} ${2:name}` |
| **Hover** | Pointer-over docs from the same tables as autocomplete: every builtin's **signature (placeholders stripped) + full contract** with its SPEC pointer, keywords and constants, the twelve builtin exception classes (when cstar raises them, exact-class catching, `.type`/`.message`), method names right of a dot (per-type contracts), the `variable.` qualifier form, and user function (`name():` / `name(params):`) and `class` declarations (declaration signature + line). |
| **Snippets** | `function`, `main`, `print`, `return`, plus the Stage 2A shapes: `if`, `ifelse`, `ifin` (membership `()` test), `while`, `for` (over `range(...)`), `foreach` (over a collection), `switch` (case + `case *:`), `pass` — plus the Stage 2B shapes: `functionargs` (`*args`/`**kwargs`), `functionnested` (nested closure), `global`, `nonlocal` — plus the Stage 2C shapes: `class`, `init` (`__init__`), `try` (try/except/`as`), `raise` — plus the 0.2.1 shape `variable` (inserts `variable.<qualifier> <name>`; the qualifier is `global` — the **module global** without a `global` decl, `parent` — the **enclosing function's** binding without `nonlocal`, or `variable.<fn>` — a **sibling/self function's** binding sharing this scope; see `docs/reference/cross-scope-access.md`) — plus the 0.2.1 Python-parity shapes `open` (open a file, iterate its lines, close it), `sorted` (sorted list with `key=`) and `isinstance` (exact-type `if` test) |
| **Live error squiggles** | Runs `cstar check` (parse + semantic analysis only — **never compiles or executes code**) debounced while you type; errors appear as red underlines with exact `line:col` messages |
| **Run button** | **▶ in the editor title bar** (and the editor right-click menu, plus Command Palette → "cstar: Run File") runs **the file you are on**: the editor the ▶ was clicked for (VSCode passes it as the command resource), falling back to the active document for palette/keybindings — saved first if dirty, so stale code never runs — via `cstar run <file>` in a reusable **cstar** terminal (real terminal, so `input()` prompts are answerable; closed terminals are recreated, so the button keeps working). The CLI is found as described under [Finding the CLI](#finding-the-cli) |
| **Editor niceties** | `#` line / `/* */` block comment toggling (Ctrl+/, `#` style) plus the `cstar.toggleLineComment` command which toggles **both `#` and `//`** (bind it via `keybindings.json`), auto-indent after `if`/`elif`/`else`/`for`/`while`/`switch`/`case`/`class`/`try`/`except`/`finally`/`main` headers and function declarations (`name():` / `name(params):`) with `else:`/`elif:`/`case`/`except:`/`finally:` dedent-back, auto-pairs for `()`, `[]`, `{}` and quotes |

## Install

From the repository root (Linux, macOS, or Git Bash on Windows):

```bash
./install-vscode-extension.sh
# then: Ctrl+Shift+P → "Developer: Reload Window"
```

The script links `editors/vscode` into your extensions folder as
`cstar-lang.cstar-lang` (a junction on Windows), so edits here take effect on
the next reload. It also removes any older link to this folder, such as the
pre-rename `pentest-lang.pentest-lang`, which would otherwise load the
extension twice. To link it by hand on Linux or macOS:

```bash
ln -s "$(pwd)/editors/vscode" ~/.vscode/extensions/cstar-lang.cstar-lang
```

Open any `.cst` file — the status bar shows **cstar**.

To make Ctrl+/ toggle both `#` and `//` comments, add to `keybindings.json`:

```json
{ "key": "ctrl+/", "command": "cstar.toggleLineComment",
  "when": "editorTextFocus && editorLangId == cstar" }
```

## Smoke test

```bash
node editors/vscode/test/smoke.js   # from the repo root (npm test also works here)
```

No network, no dependencies: it loads the grammar/snippets/package files,
tokenizes sample lines with the real TextMate regexes (bytes literals, slice
colons, keywords…), and invokes the completion provider against a stubbed
`vscode` API (keywords, 2B builtins, reserved-name suppression, method
completions after `.`, and function declarations with the block syntax). Exit
code 0 means every check passed (currently 1363).

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `cstar.executable` | `cstar` | CLI used for diagnostics and the Run button (see below) |
| `cstar.diagnostics` | `true` | Toggle live error squiggles |
| `cstar.diagnosticsDelay` | `400` | Debounce in ms before re-checking |

The pre-rename settings `pentest.executable`, `pentest.diagnostics` and
`pentest.diagnosticsDelay` still work until v0.4 and show as deprecated: each
is used only when the matching `cstar.*` setting is not set. Keybindings that
use the old command ids `pentest.run` and `pentest.toggleLineComment` keep
working too.

### Finding the CLI

Diagnostics and Run need the `cstar` CLI (run `make` in the repo root, then put
`build/cstar` on your `PATH` or link it to `~/.local/bin/cstar`). It is looked
up as:

1. a path given in `cstar.executable` (if it contains `/`), used as-is;
2. otherwise that name on `PATH`, then `~/.local/bin/<name>`, then a hard-coded
   `~/Projects/Pentest/build/<name>` developer fallback;
3. while `cstar.executable` is left at its default, the same three places for
   the legacy `pentest` name.

If none is found, a warning is shown once ("cstar CLI not found — live error
diagnostics are disabled") and highlighting, completion, hover and snippets
keep working.

**Windows:** the cstar toolchain is Linux-only until v0.4, so on Windows the
warning above is expected and Run reports that `cstar` is not recognised.
Build and run programs from WSL (see
[getting started](../../docs/guide/getting-started.md#windows-wsl)).

## For maintainers

How the extension is structured, which tables must follow language changes, and
how its smoke test works: [docs/development/editor-extension.md](../../docs/development/editor-extension.md).
The language itself is documented from [docs/README.md](../../docs/README.md).
