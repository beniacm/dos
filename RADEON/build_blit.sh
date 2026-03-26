#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "Building RBLIT (shared HW layer + blitter tests)..."
wcc386 -bt=dos -5r -ox -s -zq RADEONHW.C
wcc386 -bt=dos -5r -ox -s -zq RBLIT.C
wlink system pmodew name RBLIT file { RBLIT RADEONHW } option quiet
mv RBLIT.exe RBLIT.EXE
echo "Done: RBLIT.EXE"
