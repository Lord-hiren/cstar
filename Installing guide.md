# Installing cstar

How to install the cstar toolchain (version `0.3.0`) on Linux and on Windows.

| Platform | How cstar runs today |
|---|---|
| **Linux** (x86_64) | Natively: build from source, then run `cstar` from any terminal |
| **Windows** 10/11 | Inside **WSL** (Windows Subsystem for Linux) with Ubuntu 24.04: the same Linux toolchain, started from Windows |
| macOS | Not supported |

> **No installer yet.** cstar is installed by building it from source, which
> takes about a minute. A native Windows and Linux SDK, installed once with no
> separate C compiler, is the next milestone (v0.4).

---

## Linux

These steps were checked on Ubuntu 24.04. Other distributions work the same once
the tools listed under [Other distributions](#other-distributions) are installed.

### 1. Install the tools

```bash
sudo apt-get update
sudo apt-get install -y build-essential clang make libcurl4-openssl-dev git
```

| Tool | Why cstar needs it |
|---|---|
| `clang` | Compiles cstar itself, **and every cstar program you run or build**. Keep it on your `PATH` |
| `make`, `build-essential` | Builds the toolchain |
| `libcurl4-openssl-dev` | The HTTP builtins (`http_get`, `http_post`); every program links libcurl |
| `git` | Gets the source |
| `sha256sum` (preinstalled) | Keys the compiled-program cache. Without it cstar still works, but recompiles on every run |

### 2. Get the source

```bash
git clone --branch v0.3.0 https://github.com/Lord-hiren/cstar.git
cd cstar
```

Leave out `--branch v0.3.0` to get the latest development version instead of the
release.

### 3. Build

```bash
make
build/cstar --version     # cstar 0.3.0
```

The result, `build/cstar`, is self-contained: the runtime is embedded in it.
To run the test suite as well (381 checks, about a minute), use `make test`.

### 4. Put `cstar` on your PATH

```bash
mkdir -p ~/.local/bin
ln -sf "$PWD/build/cstar" ~/.local/bin/cstar
```

Open a new terminal, then check:

```bash
cstar --version
```

If the command is not found, add `~/.local/bin` to your `PATH`:

```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
```

Then open a new terminal again.

### 5. Run your first program

```bash
echo 'print("Hello, cstar!")' > hello.cst
cstar run hello.cst       # compiles, caches and runs: Hello, cstar!
cstar build hello.cst     # writes a standalone executable ./hello
./hello
```

### Other distributions

Install the equivalent packages with your package manager:
- `clang` (14 or newer);
- GNU `make`;
- the libcurl development headers (the package that provides `curl/curl.h`);
- `git`;
- coreutils (for `sha256sum`).

Then follow steps 2–5 above.

---

## Windows (WSL)

The cstar toolchain is a Linux program, so on Windows it runs inside WSL with
Ubuntu 24.04. This was checked on Windows 11.

### 1. Install WSL and Ubuntu

In PowerShell:

```powershell
wsl --install -d Ubuntu-24.04
```

Accept the administrator prompt and **restart Windows** when asked. Then run the
same command again. It finishes installing Ubuntu and asks you to create a Linux
user name and password.

### 2. Install the tools inside Ubuntu

```powershell
wsl -d Ubuntu-24.04 -u root -- bash -c "apt-get update && apt-get install -y build-essential clang make libcurl4-openssl-dev git"
```

### 3. Build cstar

There are two ways to keep the source. Pick one.

**A. Inside Ubuntu (recommended).** This is the same as on Linux, and fastest. Open
Ubuntu from the Start menu, or run `wsl -d Ubuntu-24.04`. Then follow
[Linux](#linux) steps 2–5 in that terminal.

**B. On a Windows drive.** Use this if you want to edit the files with Windows tools.
Clone with Git for Windows:

```powershell
git clone --branch v0.3.0 https://github.com/Lord-hiren/cstar.git
cd cstar
```

Then build and run through `wsl` from that folder (`wsl` starts in the current
folder). First save a file `hello.cst` there containing `print("Hello, cstar!")`.

```powershell
wsl -d Ubuntu-24.04 -- make
wsl -d Ubuntu-24.04 -- build/cstar --version
wsl -d Ubuntu-24.04 -- build/cstar run hello.cst
```

> **Keep LF line endings.** The repository's `.gitattributes` keeps every file LF
> on Windows too. Don't convert files to CRLF: it breaks the test suite.

Programs built with `cstar build` are Linux executables. Run them inside WSL, for
example `wsl -d Ubuntu-24.04 -- ./hello`.

---

## The VS Code extension (optional)

The extension provides:
- syntax highlighting, completion, hover help and snippets;
- live error squiggles and a ▶ Run button.

Install it from the repository folder, in a Linux terminal or in Git Bash on
Windows:

```bash
./install-vscode-extension.sh
```

Then, in VS Code, run **Developer: Reload Window** (Ctrl+Shift+P).

- **Linux:** everything works once `cstar` is on your `PATH` (step 4). Otherwise,
  set the `cstar.executable` setting to the full path of `build/cstar`.
- **Windows:** highlighting, completion, hover and snippets work. The error
  squiggles and the Run button need the `cstar` command, which runs only inside
  WSL for now, so run your programs from a WSL terminal. Full native support
  comes with the v0.4 SDK.

---

## Updating

```bash
cd cstar
git pull                  # or: git checkout <newer release tag>
make
```

The `~/.local/bin/cstar` link keeps pointing at the new build. The compiled
program cache refreshes itself when `cstar` changes.

## Uninstalling

```bash
rm ~/.local/bin/cstar          # the PATH link
rm -rf ~/.cache/cstar          # the compiled-program cache
rm -rf cstar                   # the source folder
```

For the VS Code extension, delete the `cstar-lang.cstar-lang` link in your VS
Code extensions folder.

## Troubleshooting

| Problem | Fix |
|---|---|
| `'clang' was not found on PATH` | Install `clang`. It is needed every time a program is compiled, not only to build cstar |
| `curl/curl.h: No such file or directory` while building | Install the libcurl development headers (`libcurl4-openssl-dev` on Ubuntu) |
| `cstar: command not found` | Redo [step 4](#4-put-cstar-on-your-path) and open a new terminal, or run `build/cstar` directly |
| Every run recompiles | `sha256sum` is missing, or the cache folder (`~/.cache/cstar`) is not writable |
| Tests fail on Windows with odd differences | The files were converted to CRLF. Re-clone, and keep the repository's LF line endings |

## Next steps

- [Getting started](docs/guide/getting-started.md): your first programs and where files go
- [Language tour](docs/guide/language-tour.md): the whole language, with runnable examples
- [CLI reference](docs/guide/cli.md): every `cstar` command and option
