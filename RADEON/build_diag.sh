#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "Building RDIAG (shared HW layer + hardware diagnostic)..."
wcc386 -bt=dos -5r -ox -s -zq RADEONHW.C
wcc386 -bt=dos -5r -ox -s -zq RDIAG.C
wlink system pmodew name RDIAG file { RDIAG RADEONHW } option quiet
echo "Done: RDIAG.exe"
