#!/usr/bin/env bash
#
# install-vscode-extension.sh
#
# Installs the Pentest Language VS Code extension from this repository into the
# user's VS Code extensions directory as a symlink (development install).
#
# Works on Linux, macOS, and Windows (Git Bash / WSL / MSYS2).
#
# Usage:
#   ./install-vscode-extension.sh
#
# After installing, reload VS Code so it picks up the new extension:
#   Ctrl+Shift+P  →  "Developer: Reload Window"

set -euo pipefail

# Resolve the directory containing this script (the repository root).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

EXTENSION_DIR="$SCRIPT_DIR/editors/vscode"
PACKAGE_JSON="$EXTENSION_DIR/package.json"

if [[ ! -d "$EXTENSION_DIR" ]]; then
    echo "Error: VS Code extension not found at $EXTENSION_DIR" >&2
    echo "Run this script from the Pentest repository root." >&2
    exit 1
fi

if [[ ! -f "$PACKAGE_JSON" ]]; then
    echo "Error: $PACKAGE_JSON not found." >&2
    exit 1
fi

# Read publisher and extension name from package.json so the install folder
# follows the VS Code convention: ${publisher}.${name}.
# Falls back to sensible defaults if parsing fails.
PUBLISHER="pentest-lang"
EXTENSION_NAME="pentest-lang"
if command -v grep >/dev/null 2>&1; then
    PUBLISHER="$(grep -m1 '"publisher"' "$PACKAGE_JSON" | sed -E 's/.*"publisher"\s*:\s*"([^"]+)".*/\1/')"
    EXTENSION_NAME="$(grep -m1 '"name"' "$PACKAGE_JSON" | sed -E 's/.*"name"\s*:\s*"([^"]+)".*/\1/')"
fi
EXTENSION_ID="${PUBLISHER}.${EXTENSION_NAME}"

# Determine the VS Code extensions directory per platform.
case "$(uname -s)" in
    Linux*)
        VSCODE_EXTENSIONS_DIR="$HOME/.vscode/extensions"
        ;;
    Darwin*)
        VSCODE_EXTENSIONS_DIR="$HOME/Library/Application Support/Code/Extensions"
        ;;
    CYGWIN*|MINGW*|MSYS*|Windows*)
        VSCODE_EXTENSIONS_DIR="${USERPROFILE:-$HOME}/.vscode/extensions"
        ;;
    *)
        echo "Warning: unrecognised platform; assuming $HOME/.vscode/extensions" >&2
        VSCODE_EXTENSIONS_DIR="$HOME/.vscode/extensions"
        ;;
esac

# Make sure the extensions directory exists.
mkdir -p "$VSCODE_EXTENSIONS_DIR"

# Remove any previous install of this extension (symlink or real folder).
TARGET="$VSCODE_EXTENSIONS_DIR/$EXTENSION_ID"
if [[ -L "$TARGET" || -e "$TARGET" ]]; then
    rm -rf "$TARGET"
fi

# Create the symlink pointing at the extension in this repository.
ln -s "$EXTENSION_DIR" "$TARGET"

echo "Installed Pentest Language extension:"
echo "  $TARGET"
echo "  -> $EXTENSION_DIR"
echo
echo "Reload VS Code to activate it:"
echo "  Ctrl+Shift+P  →  Developer: Reload Window"
echo
echo "The extension activates for .pen and .pent files and provides:"
echo "  syntax highlighting, snippets, autocomplete, hover docs, live diagnostics"
echo "  (pentest check), and a Run button."
