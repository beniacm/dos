#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

echo "Building DUCKHUNT..."
wcc386 -bt=dos -5r -fp5 -ox -s -zq DUCKHUNT.C
wlink system pmodew name DUCKHUNT file DUCKHUNT option quiet
echo "Done: DUCKHUNT.exe"
