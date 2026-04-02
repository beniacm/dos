#!/bin/bash
set -e

# DJGPP cross-compiler build script for RADEON suite
#
# Produces executables:
#   RADGCC.EXE   - combined demo      (RADEON.C  + RADEONHW + DOSLIB)
#   RPATGCC.EXE  - pattern demo       (RPATTERN.C + RSETUP + RADEONHW + DOSLIB)
#   RBENGCC.EXE  - benchmark demo     (RBENCH.C   + ...)
#   RFLOGCC.EXE  - flood demo         (RFLOOD.C   + ...)
#   RSPRGCC.EXE  - sprite demo        (RSPRITE.C  + ...)
#   RDUNGCC.EXE  - dune chase demo    (RDUNE.C    + ...)
#   RPLXGCC.EXE  - parallax demo      (RPLAX.C    + ...)
#   RPACGCC.EXE  - scrolling Pac-Man  (RPAC.C    + RSETUP + RADEONHW + DOSLIB)
#   RBLGCC.EXE   - blitter tests      (RBLIT.C   + RADEONHW + DOSLIB)
#   RDIGCC.EXE   - HW diagnostic      (RDIAG.C   + RADEONHW + DOSLIB)

DJGPP_ROOT="${DJGPP_ROOT:-/opt/djgpp}"

export PATH="$DJGPP_ROOT/bin:$DJGPP_ROOT/i586-pc-msdosdjgpp/bin:$PATH"
export GCC_EXEC_PREFIX="$DJGPP_ROOT/lib/gcc/"
export DJDIR="$DJGPP_ROOT/i586-pc-msdosdjgpp"

CC=i586-pc-msdosdjgpp-gcc
STRIP=i586-pc-msdosdjgpp-strip
STUBEDIT="$DJGPP_ROOT/i586-pc-msdosdjgpp/bin/stubedit"

COMMON="-O3 -ffast-math -funroll-loops -fomit-frame-pointer -Wall -Wno-unused-function"

if [ "$1" = "dosbox" ]; then
    CFLAGS="-march=pentium-mmx -mtune=prescott -mno-sse -mfpmath=387 $COMMON"
    echo "Building RADEON suite (DJGPP/GCC - DOSBox-X safe, x87 only)..."
else
    CFLAGS="-march=prescott -mfpmath=sse $COMMON"
    echo "Building RADEON suite (DJGPP/GCC - full SSE2)..."
fi

echo "  CC: $($CC --version | head -1)"
echo "  CFLAGS: $CFLAGS"

DIAG_FLAGS="-march=i586 -O2 -fomit-frame-pointer -Wall -Wno-unused-function -mfpmath=387"

# Helper: link, stubedit, strip, rename
build_exe() {
    local SRC=$1   # source .o name (without .o)
    local OUT=$2   # output .EXE name
    shift 2
    local OBJS="$@"
    local FLAGS=$CFLAGS
    # RDIAG uses special flags
    if [ "$SRC" = "rdiag" ]; then FLAGS=$DIAG_FLAGS; fi
    echo "  Linking $OUT..."
    $CC $FLAGS -o ${OUT,,} $OBJS -lm
    $STUBEDIT ${OUT,,} dpmi=cwsdpr0.exe
    $STRIP ${OUT,,}
    mv ${OUT,,} $OUT
    ls -la $OUT
}

echo ""

# Compile shared library objects once
echo "Compiling shared library..."
$CC $CFLAGS -c -o doslib.o ../DOSLIB/DOSLIB.C
$CC $CFLAGS -c -o radeonhw.o RADEONHW.C
$CC $CFLAGS -c -o rsetup.o RSETUP.C

LIB_O="doslib.o radeonhw.o"
SETUP_O="$LIB_O rsetup.o"

# --- RADGCC.EXE: combined demo ---
echo ""
echo "Compiling RADEON.C..."
$CC $CFLAGS -c -o radeon.o RADEON.C
build_exe radeon RADGCC.EXE radeon.o $LIB_O

# --- Individual standalone demos ---
for SPEC in "RPATTERN:RPATGCC" "RBENCH:RBENGCC" "RFLOOD:RFLOGCC" "RSPRITE:RSPRGCC" "RDUNE:RDUNGCC" "RPLAX:RPLXGCC" "RPAC:RPACGCC"; do
    SRC="${SPEC%%:*}"
    OUT="${SPEC##*:}"
    echo ""
    echo "Compiling ${SRC}.C..."
    OBJ=$(echo $SRC | tr '[:upper:]' '[:lower:]')
    $CC $CFLAGS -c -o ${OBJ}.o ${SRC}.C
    build_exe $OBJ ${OUT}.EXE ${OBJ}.o $SETUP_O
done

# --- RBLGCC.EXE: blitter validation ---
echo ""
echo "Compiling RBLIT.C..."
$CC $CFLAGS -c -o rblit.o RBLIT.C
build_exe rblit RBLGCC.EXE rblit.o $LIB_O

# --- RDIGCC.EXE: hardware diagnostic ---
echo ""
echo "Compiling RDIAG.C (diagnostic flags)..."
$CC $DIAG_FLAGS -c -o rdiag.o RDIAG.C
build_exe rdiag RDIGCC.EXE rdiag.o $LIB_O

rm -f doslib.o radeonhw.o rsetup.o radeon.o rblit.o rdiag.o
rm -f rpattern.o rbench.o rflood.o rsprite.o rdune.o rplax.o

echo ""
echo "All executables built. Requires CWSDPR0.EXE in the same directory or PATH."
echo ""
echo "Usage: $0          # Full SSE2 (real hardware)"
echo "       $0 dosbox   # DOSBox-X safe (x87 only)"
