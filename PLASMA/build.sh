#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "Building PLASMA..."
wcc386 -bt=dos -5r -ox -s -zq PLASMA.C
wlink system pmodew name PLASMA file PLASMA option quiet
echo "Done: PLASMA.exe"
