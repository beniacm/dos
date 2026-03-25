#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "Building RADEON..."
wcc386 -bt=dos -5r -ox -s -zq RADEON.C
wlink system pmodew name RADEON file RADEON option quiet
echo "Done: RADEON.exe"
