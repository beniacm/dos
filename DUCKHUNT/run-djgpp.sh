#!/bin/sh
# Run DUCKGCC.EXE (QUACK HUNT, DJGPP/GCC build) in DOSBox-X
#
# Build with:  ./build-djgpp.sh dosbox   (x87 only, works in DOSBox)
#              ./build-djgpp.sh           (SSE2/SSE3, real Pentium D only)
#
# CWSDPMI.EXE must be in the same directory as DUCKGCC.EXE.

DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="$(mktemp /tmp/duckhunt-dosbox-XXXXXX.conf)"

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
DUCKGCC.EXE $*
EOF

dosbox -conf "$CONF"
rm -f "$CONF"
