#!/bin/sh
# Run PLASMGCC.EXE (PLASMA, DJGPP/GCC build) in DOSBox-X
#
# Build with:  ./build-djgpp.sh dosbox   (x87 only, works in DOSBox)
#              ./build-djgpp.sh           (SSE2/SSE3, real Pentium D only)
#
# CWSDPMI.EXE must be in the same directory as PLASMGCC.EXE.
#
# On real hardware: run WCINIT.EXE first to set MTRR write-combining.
# CWSDPMI runs at ring 3 and cannot program MTRRs, so without WCINIT
# the blit speed is ~80 MB/s (UC) instead of ~700 MB/s (WC).
#
#   C:\> WCINIT.EXE
#   C:\> PLASMGCC.EXE

DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="$(mktemp /tmp/plasma-dosbox-XXXXXX.conf)"

cat > "$CONF" << EOF
[sdl]
fullscreen=false

[dosbox]
machine=svga_s3
memsize=64

[cpu]
cputype=pentium
core=normal
cycles=max

[autoexec]
mount c $DIR
c:
PLASMGCC.EXE -pmi $*
EOF

dosbox -conf "$CONF"
rm -f "$CONF"
