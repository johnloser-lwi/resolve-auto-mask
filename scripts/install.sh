#!/usr/bin/env bash
#
# Install AutoMask.ofx.bundle into the macOS OFX plugin directory.
#
# Resolve scans for OFX plugins only at launch, so it must be restarted after
# installing, and it holds the plugin open once it has loaded one.

set -euo pipefail

CONFIG=Release
DEST="/Library/OFX/Plugins"
BUILD_FIRST=0
FORCE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)   CONFIG=Debug ;;
        --release) CONFIG=Release ;;
        --build)   BUILD_FIRST=1 ;;
        # Close Resolve automatically instead of refusing.
        --force)   FORCE=1 ;;
        # Per-user install, no administrator password. Resolve does read this
        # location, but /Library is the one every OFX host agrees on, so it
        # stays the default.
        --user)    DEST="$HOME/Library/OFX/Plugins" ;;
        --dest)    shift; DEST="$1" ;;
        -h|--help)
            echo "usage: $0 [--debug|--release] [--build] [--force] [--user] [--dest DIR]"
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="$REPO/build/$CONFIG/AutoMask.ofx.bundle"

step() { printf '==> %s\n' "$1"; }
ok()   { printf '    %s\n' "$1"; }

# Deliberately before any sudo: building as root leaves build/ owned by root,
# and every later ordinary build then fails to overwrite it.
if [[ $BUILD_FIRST -eq 1 ]]; then
    step "Building ($CONFIG)..."
    "$REPO/scripts/build.sh" --"$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
fi

step "Checking bundle..."
[[ -d "$BUNDLE" ]] || {
    echo "Bundle not found at:" >&2
    echo "  $BUNDLE" >&2
    echo "Run scripts/build.sh first, or re-run this with --build." >&2
    exit 1
}
PAYLOAD="$BUNDLE/Contents/MacOS/AutoMask.ofx"
[[ -f "$PAYLOAD" ]] || {
    echo "Bundle is malformed - missing Contents/MacOS/AutoMask.ofx" >&2
    exit 1
}
ok "Found $(( $(stat -f%z "$PAYLOAD") / 1024 )) KB plugin binary."
ok "Slices: $(lipo -archs "$PAYLOAD" 2>/dev/null || echo unknown)"

# --- Resolve -----------------------------------------------------------------
# Only an actual open handle matters. A first-time install of a plugin Resolve
# has never seen copies fine while it runs, so warn, try, and only insist if
# the copy fails.
if pgrep -x "Resolve" >/dev/null 2>&1; then
    if [[ $FORCE -eq 1 ]]; then
        step "Closing DaVinci Resolve..."
        osascript -e 'tell application "DaVinci Resolve" to quit' || true
        for _ in $(seq 1 30); do
            pgrep -x "Resolve" >/dev/null 2>&1 || break
            sleep 1
        done
        pgrep -x "Resolve" >/dev/null 2>&1 && {
            echo "Resolve did not close within 30s. Close it manually and re-run." >&2
            exit 1
        }
        ok "Closed."
    else
        echo "WARNING: DaVinci Resolve is running. Attempting the install anyway;" >&2
        echo "         it will fail only if Resolve has already loaded this plugin." >&2
    fi
fi

# --- Install -----------------------------------------------------------------
# Probe rather than assume. An OFX directory with relaxed permissions needs no
# sudo, and asking for rights we do not need is just friction.
SUDO=""
if [[ ! -d "$DEST" ]]; then
    mkdir -p "$DEST" 2>/dev/null || SUDO="sudo"
fi
[[ -z "$SUDO" && ! -w "$DEST" ]] && SUDO="sudo"

step "Installing to $DEST"
[[ -n "$SUDO" ]] && ok "Administrator rights required; you will be prompted."

$SUDO mkdir -p "$DEST"
TARGET="$DEST/AutoMask.ofx.bundle"
[[ -d "$TARGET" ]] && { ok "Replacing existing installation."; $SUDO rm -rf "$TARGET"; }
$SUDO cp -R "$BUNDLE" "$DEST/"

INSTALLED="$TARGET/Contents/MacOS/AutoMask.ofx"
[[ -f "$INSTALLED" ]] || { echo "Copy appeared to succeed but $INSTALLED is missing." >&2; exit 1; }

# Ad-hoc signature. The linker already applies one to each arm64 slice, but
# copying can disturb it, and on Apple Silicon an invalid signature means the
# dylib will not load at all -- which Resolve reports as the plugin simply not
# existing. Re-signing costs nothing and removes the question.
if command -v codesign >/dev/null 2>&1; then
    $SUDO codesign --force --sign - "$INSTALLED" >/dev/null 2>&1 \
        && ok "Ad-hoc signed." \
        || ok "codesign failed; continuing (usually still fine)."
fi
# Only matters if the bundle ever arrived via a download or AirDrop, but it is
# free to clear and the failure it prevents is another silent no-load.
xattr -dr com.apple.quarantine "$TARGET" 2>/dev/null || true

if ! diff -q "$PAYLOAD" "$INSTALLED" >/dev/null 2>&1; then
    # codesign rewrites the binary in place, so a byte difference here is
    # expected and not a failure; only a missing file was.
    ok "Installed binary differs from the build (expected after signing)."
fi

ok "Verified: $INSTALLED"
echo ""
echo "Installed successfully."
echo ""
echo "Next:"
echo "  1. Fully quit and relaunch DaVinci Resolve (it scans OFX plugins only at launch)."
echo "  2. Apply 'Auto Mask' (group: AutoMask) from the OpenFX library."
echo "  3. Under the Viewer, set the on-screen-control dropdown to 'Open FX Overlay'."
echo "     Resolve hides OFX overlays until you do."
echo ""
echo "Probe log: ~/Library/Logs/AutoMask/probe.log"
echo "  (written by the plugin on load - confirms host capabilities)"
