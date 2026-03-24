#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "Building RBLIT..."
wcc386 -bt=dos -3r -ox -s -zq RBLIT.C
wlink system pmodew name RBLIT file RBLIT option quiet
echo "Done: RBLIT.exe"
