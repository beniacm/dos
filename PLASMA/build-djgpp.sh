#!/bin/bash
set -e

# DJGPP cross-compiler build script for PLASMA
# Target: Pentium D (Prescott core) - uses SSE/SSE2/SSE3
#
# Uses CWSDPR0.EXE (ring-0 DPMI server) so the program has full access
# to MTRR/PAT/CR4 — no need for a separate WCINIT helper.
DJGPP_ROOT="${DJGPP_ROOT:-/opt/djgpp}"

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
    echo "Building PLASMA (DJGPP/GCC - DOSBox-X safe, x87 only)..."
else
    # Full Pentium D: SSE2/SSE3, all float/double via SSE
    # Ring 0 via CWSDPR0 for MTRR WC + CR4.OSFXSR
    CFLAGS="-march=prescott -mfpmath=sse $COMMON"
    echo "Building PLASMA (DJGPP/GCC - Pentium D, full SSE2/SSE3)..."
fi

echo "  CC: $($CC --version | head -1)"
echo "  CFLAGS: $CFLAGS"

# --- PLASMA ---
$CC $CFLAGS -o plasmgcc.exe PLASMA.C VGA.C WAVE.C -lm
$STUBEDIT plasmgcc.exe dpmi=cwsdpr0.exe
$STRIP plasmgcc.exe
mv plasmgcc.exe PLASMGCC.EXE
ls -la PLASMGCC.EXE
echo "Done: PLASMGCC.EXE"

# --- WATER ---
echo ""
echo "Building WATER (lit water surface demo)..."
$CC $CFLAGS -o watrgcc.exe WATERDMO.C VGA.C WAVE.C WATER.C -lm
$STUBEDIT watrgcc.exe dpmi=cwsdpr0.exe
$STRIP watrgcc.exe
mv watrgcc.exe WATRGCC.EXE
ls -la WATRGCC.EXE
echo "Done: WATRGCC.EXE"

# --- WATERDRP ---
echo ""
echo "Building WATERDRP (water drop ripple demo)..."
$CC $CFLAGS -o wdrpgcc.exe WATERDRP.C VGA.C -lm
$STUBEDIT wdrpgcc.exe dpmi=cwsdpr0.exe
$STRIP wdrpgcc.exe
mv wdrpgcc.exe WDRPGCC.EXE
ls -la WDRPGCC.EXE
echo "Done: WDRPGCC.EXE"

echo ""
echo "Requires CWSDPR0.EXE in the same directory or PATH."
echo ""
echo "Usage: $0          # Full Pentium D SSE2/SSE3 (real hardware only)"
echo "       $0 dosbox   # DOSBox-X safe (x87 only, works in any DOSBox)"
