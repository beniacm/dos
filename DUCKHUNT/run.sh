#!/bin/sh
# Run DUCKHUNT.EXE (QUACK HUNT) in DOSBox
DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="$(mktemp /tmp/duckhunt-dosbox-XXXXXX.conf)"

cat > "$CONF" << EOF
[sdl]
fullscreen=false

[dosbox]
machine=svga_s3
memsize=32

[cpu]
cycles=max

[autoexec]
mount c $DIR
c:
DUCKHUNT.EXE $*
EOF

dosbox -conf "$CONF"
rm -f "$CONF"
