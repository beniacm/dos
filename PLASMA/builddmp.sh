#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "Building VBEDUMP..."
wcc386 -bt=dos -3r -ox -s -zq VBEDUMP.C
wlink system pmodew name VBEDUMP file VBEDUMP option quiet
mv VBEDUMP.exe VBEDUMP.EXE
echo "Done: VBEDUMP.EXE"
