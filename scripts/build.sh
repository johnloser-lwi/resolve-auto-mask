#!/usr/bin/env bash
#
# Build AutoMask.ofx.bundle on macOS.
#
# The Windows counterpart is build.ps1. They are separate scripts rather than
# one cross-platform launcher because the interesting parts do not overlap:
# that one has to locate a Visual Studio toolchain, this one has to decide
# which architectures to fatten the binary with.

set -euo pipefail

CONFIG=Release
CLEAN=0
EXTRA=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)   CONFIG=Debug ;;
        --release) CONFIG=Release ;;
        --clean)   CLEAN=1 ;;
        # Native-only. Faster to build and all you need for your own machine;
        # the universal default exists so the bundle also runs on the other
        # kind of Mac.
        --native)  EXTRA+=("-DAUTOMASK_UNIVERSAL=OFF") ;;
        -h|--help)
            echo "usage: $0 [--debug|--release] [--clean] [--native]"
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO/build"

command -v cmake >/dev/null 2>&1 || {
    echo "cmake not found. Install it with:  brew install cmake" >&2
    exit 1
}
xcode-select -p >/dev/null 2>&1 || {
    echo "Xcode command line tools not found. Install them with:" >&2
    echo "  xcode-select --install" >&2
    exit 1
}

if [[ $CLEAN -eq 1 && -d "$BUILD" ]]; then
    echo "Removing $BUILD"
    rm -rf "$BUILD"
fi

# Ninja when it is there, Makefiles when it is not. Both are single-config, so
# the build type goes in at configure time.
GEN=("-G" "Unix Makefiles")
command -v ninja >/dev/null 2>&1 && GEN=("-G" "Ninja")

echo "Generator:     ${GEN[1]}"
echo "Configuration: $CONFIG"

cmake -S "$REPO" -B "$BUILD" "${GEN[@]}" \
      -DCMAKE_BUILD_TYPE="$CONFIG" \
      "${EXTRA[@]}"

cmake --build "$BUILD" --parallel "$(sysctl -n hw.ncpu)"

BUNDLE="$BUILD/$CONFIG/AutoMask.ofx.bundle"
BIN="$BUNDLE/Contents/MacOS/AutoMask.ofx"

echo ""
echo "Bundle: $BUNDLE"
if [[ -f "$BIN" ]]; then
    # Worth printing: a bundle missing the slice for the Mac that loads it is
    # not an error anywhere, it is simply a plugin the host never lists.
    echo -n "Slices: "
    lipo -archs "$BIN" 2>/dev/null || echo "(lipo could not read it)"
fi
echo ""
echo "Run the tests with:  ctest --test-dir \"$BUILD\" --output-on-failure"
