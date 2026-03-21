#!/bin/sh
# Run PLASMGCC.EXE (PLASMA, DJGPP/GCC build) in DOSBox-X
#
# Build with:  ./build-djgpp.sh dosbox   (x87 only, works in DOSBox)
#              ./build-djgpp.sh           (SSE2/SSE3, real Pentium D only)
#
# CWSDPR0.EXE must be in the same directory as PLASMGCC.EXE.
# (CWSDPR0 is the ring-0 DPMI server — enables MTRR WC + SSE setup)
#
# On real hardware, just run PLASMGCC.EXE directly — no WCINIT needed.

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
