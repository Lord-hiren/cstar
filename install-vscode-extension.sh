#!/usr/bin/env bash
#
# install-vscode-extension.sh
#
# Installs the cstar Language VS Code extension from this repository into the
# user's VS Code extensions directory as a link (development install), so
# changes under editors/vscode take effect after "Developer: Reload Window".
#
# Works on Linux, macOS and Windows (Git Bash / MSYS2). On Windows the link is
# a directory junction, which needs no administrator rights.
#
# Usage:
#   ./install-vscode-extension.sh
#
# To uninstall, delete the cstar-lang.cstar-lang link from the extensions
# directory (this never touches the repository).

set -euo pipefail

# Resolve the directory containing this script (the repository root).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

EXTENSION_DIR="$SCRIPT_DIR/editors/vscode"
PACKAGE_JSON="$EXTENSION_DIR/package.json"

if [[ ! -d "$EXTENSION_DIR" ]]; then
    echo "Error: VS Code extension not found at $EXTENSION_DIR" >&2
    echo "Keep this script in the cstar repository root." >&2
    exit 1
fi

if [[ ! -f "$PACKAGE_JSON" ]]; then
    echo "Error: $PACKAGE_JSON not found." >&2
    exit 1
fi

# Read publisher and extension name from package.json so the install folder
# follows the VS Code convention: ${publisher}.${name}. The top-level fields
# come first in the file. Falls back to the known id if parsing fails.
PUBLISHER="cstar-lang"
EXTENSION_NAME="cstar-lang"
if command -v grep >/dev/null 2>&1; then
    PUBLISHER="$(grep -m1 '"publisher"' "$PACKAGE_JSON" | sed -E 's/.*"publisher"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/')"
    EXTENSION_NAME="$(grep -m1 '"name"' "$PACKAGE_JSON" | sed -E 's/.*"name"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/')"
fi
EXTENSION_ID="${PUBLISHER}.${EXTENSION_NAME}"

# Determine the VS Code extensions directory per platform.
ON_WINDOWS=""
case "$(uname -s)" in
    Linux*|Darwin*)
        VSCODE_EXTENSIONS_DIR="$HOME/.vscode/extensions"
        ;;
    CYGWIN*|MINGW*|MSYS*)
        ON_WINDOWS=1
        PROFILE_DIR="${USERPROFILE:-$HOME}"
        if command -v cygpath >/dev/null 2>&1; then
            PROFILE_DIR="$(cygpath -u "$PROFILE_DIR")"
        fi
        VSCODE_EXTENSIONS_DIR="$PROFILE_DIR/.vscode/extensions"
        ;;
    *)
        echo "Warning: unrecognised platform; assuming $HOME/.vscode/extensions" >&2
        VSCODE_EXTENSIONS_DIR="$HOME/.vscode/extensions"
        ;;
esac

# Make sure the extensions directory exists.
mkdir -p "$VSCODE_EXTENSIONS_DIR"

# The physical location of a directory or link, or "" if it does not resolve.
physical() { (cd "$1" 2>/dev/null && pwd -P) || true; }
EXTENSION_REAL="$(physical "$EXTENSION_DIR")"

# Remove any previous install of this extension (link or real folder). On a
# link (or junction) rm removes the link itself, never the repository files.
TARGET="$VSCODE_EXTENSIONS_DIR/$EXTENSION_ID"
if [[ -L "$TARGET" || -e "$TARGET" ]]; then
    rm -rf "$TARGET"
fi

# Before the rename to cstar the extension was installed as
# pentest-lang.pentest-lang (or, by hand, pentest.pentest-lang-0.2.1). A
# leftover link to this same folder would load the extension twice, so every
# other link in the extensions directory that resolves to editors/vscode is
# removed. Anything else is never deleted: a legacy-named folder or a link to
# another checkout is only reported.
shopt -s nullglob
for entry in "$VSCODE_EXTENSIONS_DIR"/*; do
    if [[ -L "$entry" && -n "$EXTENSION_REAL" && "$(physical "$entry")" == "$EXTENSION_REAL" ]]; then
        rm -f "$entry"
        echo "Removed an old link to this extension: $entry"
        continue
    fi
    case "$(basename "$entry")" in
        pentest-lang.pentest-lang*|pentest.pentest-lang*)
            echo "Warning: $entry is an older Pentest extension install that this script did not remove." >&2
            echo "         VS Code would load it next to $EXTENSION_ID; delete it if you no longer need it." >&2
            ;;
    esac
done
shopt -u nullglob

# Link the extension in this repository into the extensions directory. Git
# Bash's ln -s silently copies unless symlinks are enabled, so on Windows a
# junction is used instead.
LINKED=""
if [[ -n "$ON_WINDOWS" ]] && command -v cygpath >/dev/null 2>&1; then
    if MSYS_NO_PATHCONV=1 "${COMSPEC:-cmd.exe}" /c mklink /J \
            "$(cygpath -w "$TARGET")" "$(cygpath -w "$EXTENSION_DIR")" >/dev/null; then
        LINKED=1
    else
        echo "Warning: could not create a junction; falling back to ln -s." >&2
    fi
fi
if [[ -z "$LINKED" ]]; then
    ln -s "$EXTENSION_DIR" "$TARGET"
fi

echo "Installed the cstar Language extension:"
echo "  $TARGET"
echo "  -> $EXTENSION_DIR"
if [[ ! -L "$TARGET" ]]; then
    echo
    echo "Note: this is a copy, not a link. Re-run this script after changing editors/vscode."
fi
echo
echo "Reload VS Code to activate it:"
echo "  Ctrl+Shift+P  →  Developer: Reload Window"
echo
echo "The extension activates for .cst files (and legacy .pen/.pent until v0.4) and"
echo "provides syntax highlighting, snippets, autocomplete, hover docs, live"
echo "diagnostics (cstar check) and a Run button."
if [[ -n "$ON_WINDOWS" ]]; then
    echo
    echo "On Windows, diagnostics and Run need the cstar CLI, which is Linux-only until"
    echo "v0.4: VS Code warns once that it cannot find it, and everything else works."
fi
