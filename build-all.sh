#!/bin/bash
# Build all DOS demo binaries in this repo.
# Requires: OpenWatcom 2 at /opt/watcom, DJGPP at /opt/djgpp
# Usage:
#   ./build-all.sh           # OpenWatcom builds only
#   ./build-all.sh djgpp     # DJGPP/GCC builds only
#   ./build-all.sh all       # Both
#   ./build-all.sh dosbox    # DJGPP dosbox-safe builds

set -e

REPO="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:-watcom}"

export WATCOM="${WATCOM:-/opt/watcom}"
export DJGPP_ROOT="${DJGPP_ROOT:-/opt/djgpp}"

# --- Validate tools ---
if [ "$MODE" = "watcom" ] || [ "$MODE" = "all" ]; then
    if [ ! -x "$WATCOM/binl64/wcc386" ]; then
        echo "ERROR: OpenWatcom not found at $WATCOM"
        echo "  Install to /opt/watcom or set WATCOM env var."
        exit 1
    fi
fi

if [ "$MODE" = "djgpp" ] || [ "$MODE" = "dosbox" ] || [ "$MODE" = "all" ]; then
    if [ ! -x "$DJGPP_ROOT/bin/i586-pc-msdosdjgpp-gcc" ]; then
        echo "ERROR: DJGPP not found at $DJGPP_ROOT"
        echo "  Install to /opt/djgpp or set DJGPP_ROOT env var."
        exit 1
    fi
fi

build_watcom() {
    local dir="$1"
    local script="$2"
    echo ""
    echo "=== [OpenWatcom] $dir ==="
    cd "$REPO/$dir"
    bash "$script"
}

build_djgpp() {
    local dir="$1"
    local dosbox_arg="$2"
    echo ""
    echo "=== [DJGPP] $dir ==="
    cd "$REPO/$dir"
    bash build-djgpp.sh $dosbox_arg
}

DOSBOX_ARG=""
[ "$MODE" = "dosbox" ] && DOSBOX_ARG="dosbox"

if [ "$MODE" = "watcom" ] || [ "$MODE" = "all" ]; then
    build_watcom VESADEMO  build.sh
    build_watcom RADEON    build.sh
    build_watcom PLASMA    build.sh
    build_watcom DUCKHUNT  build.sh
fi

if [ "$MODE" = "djgpp" ] || [ "$MODE" = "dosbox" ] || [ "$MODE" = "all" ]; then
    build_djgpp PLASMA  "$DOSBOX_ARG"
    build_djgpp DUCKHUNT "$DOSBOX_ARG"
    build_djgpp RADEON "$DOSBOX_ARG"
fi

echo ""
echo "=== Build complete ==="
