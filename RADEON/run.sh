#!/bin/bash
# Run RADEON demo in DOSBox (for build testing only — 2D GPU accel
# requires real Radeon X1300 Pro hardware, not emulation)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

exec dosbox \
  -c "mount c ." \
  -c "c:" \
  -c "RADEON.EXE" \
  -c "pause" \
  -c "exit"
