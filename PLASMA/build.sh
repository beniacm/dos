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
wlink system pmodew name PLASMA file PLASMA option quiet
echo "Done: PLASMA.exe"

echo "Building MTRRDIAG..."
wcc386 $CFLAGS MTRRDIAG.C
wlink system pmodew name MTRRDIAG file MTRRDIAG option quiet
echo "Done: MTRRDIAG.exe"

echo "Building WCINIT..."
wcc386 $CFLAGS WCINIT.C
wlink system pmodew name WCINIT file WCINIT option quiet
echo "Done: WCINIT.exe"
