#!/bin/bash
set -e

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$WATCOM/binw:$PATH
export INCLUDE=$WATCOM/h

WCC="wcc386 -bt=dos -5r -ox -s -zq"

echo "Building RADEON suite (OpenWatcom)..."

# Compile shared library objects once
echo "  DOSLIB.C..."
$WCC ../DOSLIB/DOSLIB.C
echo "  RADEONHW.C..."
$WCC RADEONHW.C
echo "  RSETUP.C..."
$WCC RSETUP.C

LIB_OBJ="DOSLIB RADEONHW"
SETUP_OBJ="$LIB_OBJ RSETUP"

# --- Combined demo (RADEON.EXE) ---
echo "  RADEON.C..."
$WCC RADEON.C
wlink system pmodew name RADEON file { RADEON $LIB_OBJ } option quiet
mv RADEON.exe RADEON.EXE
echo "  -> RADEON.EXE"

# --- Individual demos ---
for DEMO in RPATTERN RBENCH RFLOOD RSPRITE RDUNE RPLAX RPAC; do
    echo "  ${DEMO}.C..."
    $WCC ${DEMO}.C
    wlink system pmodew name $DEMO file { $DEMO $SETUP_OBJ } option quiet
    mv ${DEMO}.exe ${DEMO}.EXE
    echo "  -> ${DEMO}.EXE"
done

# --- RBLIT (blitter validation, uses RADEONHW directly) ---
echo "  RBLIT.C..."
$WCC RBLIT.C
wlink system pmodew name RBLIT file { RBLIT $LIB_OBJ } option quiet
mv RBLIT.exe RBLIT.EXE
echo "  -> RBLIT.EXE"

# --- RDIAG (HW diagnostics, uses RADEONHW directly) ---
echo "  RDIAG.C..."
$WCC RDIAG.C
wlink system pmodew name RDIAG file { RDIAG $LIB_OBJ } option quiet
mv RDIAG.exe RDIAG.EXE
echo "  -> RDIAG.EXE"

rm -f *.obj 2>/dev/null
echo "Done: 10 executables built."
