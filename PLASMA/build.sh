#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

# -6r   = Pentium Pro+ scheduling (register calling convention)
# -ox   = Maximum optimization (-obmiler -s)
# -oh   = Enable expensive optimizations (better register allocation)
# -on   = Relax floating-point constraints (like -ffast-math)
# -zq   = Quiet mode
CFLAGS="-bt=dos -6r -ox -oh -on -zq"

echo "Building PLASMA..."
wcc386 $CFLAGS PLASMA.C
wcc386 $CFLAGS VGA.C
wcc386 $CFLAGS WAVE.C
wlink system pmodew name PLASMA file PLASMA,VGA,WAVE option quiet
mv PLASMA.exe PLASMA.EXE
echo "Done: PLASMA.EXE"

echo "Building WATER..."
wcc386 $CFLAGS WATERDMO.C
wcc386 $CFLAGS WATER.C
wlink system pmodew name WATER file WATERDMO,VGA,WAVE,WATER option quiet
mv WATER.exe WATER.EXE
echo "Done: WATER.EXE"

echo "Building WATERDRP..."
wcc386 $CFLAGS WATERDRP.C
wlink system pmodew name WATERDRP file WATERDRP,VGA option quiet
mv WATERDRP.exe WATERDRP.EXE
echo "Done: WATERDRP.EXE"

echo "Building MTRRDIAG..."
wcc386 $CFLAGS MTRRDIAG.C
wlink system pmodew name MTRRDIAG file MTRRDIAG option quiet
mv MTRRDIAG.exe MTRRDIAG.EXE
echo "Done: MTRRDIAG.EXE"

echo "Building WCINIT..."
wcc386 $CFLAGS WCINIT.C
wlink system pmodew name WCINIT file WCINIT option quiet
mv WCINIT.exe WCINIT.EXE
echo "Done: WCINIT.EXE"
rm -f PLASMA.o VGA.o WAVE.o WATERDMO.o WATER.o WATERDRP.o MTRRDIAG.o WCINIT.o
