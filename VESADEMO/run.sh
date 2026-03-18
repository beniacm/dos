#!/bin/sh
# Run VESADEMO.EXE in DOSBox
DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="$(mktemp /tmp/vesademo-dosbox-XXXXXX.conf)"

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
VESADEMO.EXE
EOF

dosbox-x -conf "$CONF"
rm -f "$CONF"
