#!/bin/sh
# Run PLASMA.EXE in DOSBox
DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="$(mktemp /tmp/plasma-dosbox-XXXXXX.conf)"

cat > "$CONF" << EOF
[sdl]
fullscreen=false

[dosbox]
machine=svga_s3
memsize=32

[cpu]
cycles=auto

[autoexec]
mount c $DIR
c:
PLASMA.EXE -pmi
EOF

dosbox -conf "$CONF"
rm -f "$CONF"
