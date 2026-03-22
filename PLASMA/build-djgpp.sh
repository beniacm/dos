#!/bin/bash
set -e

# DJGPP cross-compiler build script for PLASMA
# Target: Pentium D (Prescott core) - uses SSE/SSE2/SSE3
#
# Uses CWSDPR0.EXE (ring-0 DPMI server) so the program has full access
# to MTRR/PAT/CR4 — no need for a separate WCINIT helper.
DJGPP_ROOT="$(cd "$(dirname "$0")/../djgpp" && pwd)"

export PATH="$DJGPP_ROOT/bin:$DJGPP_ROOT/i586-pc-msdosdjgpp/bin:$PATH"
export GCC_EXEC_PREFIX="$DJGPP_ROOT/lib/gcc/"
export DJDIR="$DJGPP_ROOT/i586-pc-msdosdjgpp"

CC=i586-pc-msdosdjgpp-gcc
STRIP=i586-pc-msdosdjgpp-strip
STUBEDIT="$DJGPP_ROOT/i586-pc-msdosdjgpp/bin/stubedit"

# Common flags
COMMON="-O3 -ffast-math -funroll-loops -fomit-frame-pointer -Wall -Wno-unused-function"

if [ "$1" = "dosbox" ]; then
    # DOSBox-X safe: x87 FPU only, no SSE (DOSBox-X DPMI runs ring 3,
    # can't set CR4.OSFXSR → any SSE instruction = GPF).
    # Still benefits from GCC 12's superior optimization vs Watcom.
    CFLAGS="-march=pentium-mmx -mtune=prescott -mno-sse -mfpmath=387 $COMMON"
    OUT=PLASMGCC.EXE
    echo "Building PLASMA (DJGPP/GCC - DOSBox-X safe, x87 only)..."
else
    # Full Pentium D: SSE2/SSE3, all float/double via SSE
    # Ring 0 via CWSDPR0 for MTRR WC + CR4.OSFXSR
    CFLAGS="-march=prescott -mfpmath=sse $COMMON"
    OUT=PLASMGCC.EXE
    echo "Building PLASMA (DJGPP/GCC - Pentium D, full SSE2/SSE3)..."
fi

echo "  CC: $($CC --version | head -1)"
echo "  CFLAGS: $CFLAGS"

$CC $CFLAGS -o $OUT PLASMA.C VGA.C WAVE.C -lm

# Patch the stub to load CWSDPR0 (ring-0 DPMI server) instead of CWSDPMI
echo "Patching DPMI server to CWSDPR0 (ring 0)..."
$STUBEDIT $OUT dpmi=CWSDPR0

echo "Stripping..."
$STRIP $OUT

ls -la $OUT
echo "Done: $OUT (uses CWSDPR0.EXE for ring-0 access)"
echo ""
echo "Requires CWSDPR0.EXE in the same directory or PATH."
echo ""
echo "Usage: $0          # Full Pentium D SSE2/SSE3 (real hardware only)"
echo "       $0 dosbox   # DOSBox-X safe (x87 only, works in any DOSBox)"
