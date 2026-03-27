#!/bin/bash
set -e

# DJGPP cross-compiler build script for RADEON
# Target: Pentium (i586) with optional SSE2 for dosbox-safe builds.
#
# Produces three binaries:
#   RADGCC.EXE  - main demo       (RADEON.C  + RADEONHW.C)
#   RBLGCC.EXE  - blitter tests   (RBLIT.C   + RADEONHW.C)
#   RDIGCC.EXE  - HW diagnostic   (RDIAG.C   + RADEONHW.C)

DJGPP_ROOT="${DJGPP_ROOT:-/opt/djgpp}"

export PATH="$DJGPP_ROOT/bin:$DJGPP_ROOT/i586-pc-msdosdjgpp/bin:$PATH"
export GCC_EXEC_PREFIX="$DJGPP_ROOT/lib/gcc/"
export DJDIR="$DJGPP_ROOT/i586-pc-msdosdjgpp"

CC=i586-pc-msdosdjgpp-gcc
STRIP=i586-pc-msdosdjgpp-strip
STUBEDIT="$DJGPP_ROOT/i586-pc-msdosdjgpp/bin/stubedit"

COMMON="-O3 -ffast-math -funroll-loops -fomit-frame-pointer -Wall -Wno-unused-function"

if [ "$1" = "dosbox" ]; then
    # DOSBox-X safe: x87 FPU only, no SSE
    # (DOSBox-X DPMI runs ring 3; SSE needs CR4.OSFXSR which ring 3 can't set)
    CFLAGS="-march=pentium-mmx -mtune=prescott -mno-sse -mfpmath=387 $COMMON"
    echo "Building RADEON suite (DJGPP/GCC - DOSBox-X safe, x87 only)..."
else
    # Full Pentium 4 / compatible: SSE2 capable
    CFLAGS="-march=prescott -mfpmath=sse $COMMON"
    echo "Building RADEON suite (DJGPP/GCC - full SSE2)..."
fi

echo "  CC: $($CC --version | head -1)"
echo "  CFLAGS: $CFLAGS"

# RDIAG does not need floating-point math beyond what the diagnostics use;
# use -march=i586 -O2 for it so it runs on the widest range of hardware.
DIAG_FLAGS="-march=i586 -O2 -fomit-frame-pointer -Wall -Wno-unused-function -mfpmath=387"

# Build shared hardware layer once
echo ""
echo "Compiling RADEONHW.C..."
$CC $CFLAGS -c -o radeonhw.o RADEONHW.C

# --- RADGCC.EXE: main demo ---
echo "Compiling RADEON.C..."
$CC $CFLAGS -c -o radeon.o RADEON.C
echo "Linking RADGCC.EXE..."
$CC $CFLAGS -o radgcc.exe radeon.o radeonhw.o -lm
$STUBEDIT radgcc.exe dpmi=cwsdpr0.exe
$STRIP radgcc.exe
mv radgcc.exe RADGCC.EXE
ls -la RADGCC.EXE
echo "Done: RADGCC.EXE"

# --- RBLGCC.EXE: blitter validation ---
echo ""
echo "Compiling RBLIT.C..."
$CC $CFLAGS -c -o rblit.o RBLIT.C
echo "Linking RBLGCC.EXE..."
$CC $CFLAGS -o rblgcc.exe rblit.o radeonhw.o -lm
$STUBEDIT rblgcc.exe dpmi=cwsdpr0.exe
$STRIP rblgcc.exe
mv rblgcc.exe RBLGCC.EXE
ls -la RBLGCC.EXE
echo "Done: RBLGCC.EXE"

# --- RDIGCC.EXE: hardware diagnostic ---
echo ""
echo "Compiling RDIAG.C (diagnostic flags)..."
$CC $DIAG_FLAGS -c -o rdiag.o RDIAG.C
# radeonhw.o was compiled with $CFLAGS (higher opt) -- that is fine for linking
echo "Linking RDIGCC.EXE..."
$CC $DIAG_FLAGS -o rdigcc.exe rdiag.o radeonhw.o -lm
$STUBEDIT rdigcc.exe dpmi=cwsdpr0.exe
$STRIP rdigcc.exe
mv rdigcc.exe RDIGCC.EXE
ls -la RDIGCC.EXE
echo "Done: RDIGCC.EXE"

rm -f radeonhw.o radeon.o rblit.o rdiag.o

echo ""
echo "Requires CWSDPR0.EXE in the same directory or PATH."
echo ""
echo "Usage: $0          # Full SSE2 (real hardware)"
echo "       $0 dosbox   # DOSBox-X safe (x87 only)"
