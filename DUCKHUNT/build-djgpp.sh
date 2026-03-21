#!/bin/bash
set -e

# DJGPP cross-compiler build script for DUCKHUNT (Quack Hunt)
# Target: Pentium D (Prescott core) - uses SSE/SSE2/SSE3
DJGPP_ROOT="$(cd "$(dirname "$0")/../djgpp" && pwd)"

export PATH="$DJGPP_ROOT/bin:$DJGPP_ROOT/i586-pc-msdosdjgpp/bin:$PATH"
export GCC_EXEC_PREFIX="$DJGPP_ROOT/lib/gcc/"
export DJDIR="$DJGPP_ROOT/i586-pc-msdosdjgpp"

CC=i586-pc-msdosdjgpp-gcc
STRIP=i586-pc-msdosdjgpp-strip

# Common flags
COMMON="-O3 -ffast-math -funroll-loops -fomit-frame-pointer -Wall -Wno-unused-function"

if [ "$1" = "dosbox" ]; then
    # DOSBox-X safe: x87 FPU only, no SSE (DOSBox-X DPMI runs ring 3,
    # can't set CR4.OSFXSR → any SSE instruction = GPF).
    # Still benefits from GCC 12's superior optimization vs Watcom.
    CFLAGS="-march=pentium-mmx -mtune=prescott -mno-sse -mfpmath=387 $COMMON"
    OUT=DUCKGCC.EXE
    echo "Building DUCKHUNT (DJGPP/GCC - DOSBox-X safe, x87 only)..."
else
    # Full Pentium D: SSE2/SSE3, all float/double via SSE
    # For real hardware only (needs ring 0 for CR4.OSFXSR)
    CFLAGS="-march=prescott -mfpmath=sse $COMMON"
    OUT=DUCKGCC.EXE
    echo "Building DUCKHUNT (DJGPP/GCC - Pentium D, full SSE2/SSE3)..."
fi

echo "  CC: $($CC --version | head -1)"
echo "  CFLAGS: $CFLAGS"

$CC $CFLAGS -o $OUT DUCKHUNT.C -lm

echo "Stripping..."
$STRIP $OUT

ls -la $OUT
echo "Done: $OUT"
echo ""
echo "Usage: $0          # Full Pentium D SSE2/SSE3 (real hardware only)"
echo "       $0 dosbox   # DOSBox-X safe (x87 only, works in any DOSBox)"
