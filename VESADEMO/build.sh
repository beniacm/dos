#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$PATH
export INCLUDE=$WATCOM/h

echo "Building VESADEMO..."
wcc386 -bt=dos -3r -ox -s -zq VESADEMO.C
wlink system dos4g name VESADEMO file VESADEMO option quiet
echo "Done: VESADEMO.exe"
