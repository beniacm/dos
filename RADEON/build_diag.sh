#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "Building RDIAG..."
wcc386 -bt=dos -3r -ox -s -zq RDIAG.C
wlink system pmodew name RDIAG file RDIAG option quiet
echo "Done: RDIAG.exe"
