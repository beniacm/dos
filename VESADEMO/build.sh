#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "Building VESADEMO..."
wcc386 -bt=dos -3r -ox -s -zq VESADEMO.C
wlink system pmodew name VESADEMO file VESADEMO option quiet
mv VESADEMO.exe VESADEMO.EXE
rm -f VESADEMO.o
echo "Done: VESADEMO.EXE"
