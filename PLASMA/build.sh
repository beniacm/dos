#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$PATH
export INCLUDE=$WATCOM/h

echo "Building PLASMA..."
wcc386 -bt=dos -3r -ox -s -zq PLASMA.C
wlink system dos4g name PLASMA file PLASMA option quiet
echo "Done: PLASMA.exe"
