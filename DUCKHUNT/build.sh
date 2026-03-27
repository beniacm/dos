#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "Building DUCKHUNT (8bpp)..."
wcc386 -bt=dos -5r -fp5 -ox -s -zq DUCKHUNT.C
wlink system pmodew name DUCKHUNT file DUCKHUNT option quiet
mv DUCKHUNT.exe DUCKHUNT.EXE
rm -f DUCKHUNT.o
echo "Done: DUCKHUNT.EXE"

echo "Building DUCK16 (16bpp highcolor)..."
wcc386 -bt=dos -5r -fp5 -ox -s -zq DUCKHUNTHC.C
wlink system pmodew name DUCK16 file DUCKHUNTHC option quiet
mv DUCK16.exe DUCK16.EXE
rm -f DUCKHUNTHC.o
echo "Done: DUCK16.EXE"
