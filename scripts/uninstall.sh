#!/usr/bin/env bash
# Remove AutoMask.ofx.bundle from the macOS OFX plugin directory.

set -euo pipefail

DEST="/Library/OFX/Plugins"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --user) DEST="$HOME/Library/OFX/Plugins" ;;
        --dest) shift; DEST="$1" ;;
        -h|--help) echo "usage: $0 [--user] [--dest DIR]"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

TARGET="$DEST/AutoMask.ofx.bundle"
if [[ ! -d "$TARGET" ]]; then
    echo "Not installed at $TARGET - nothing to do."
    exit 0
fi

SUDO=""
[[ -w "$DEST" ]] || SUDO="sudo"

echo "==> Removing $TARGET"
$SUDO rm -rf "$TARGET"
echo "    Removed. Restart DaVinci Resolve for it to notice."
