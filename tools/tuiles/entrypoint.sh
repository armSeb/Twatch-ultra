#!/bin/bash
# MapLibre Native dessine avec OpenGL : écran virtuel Xvfb, comme TileServer GL.
export DISPLAY=:99
rm -f /tmp/.X99-lock
Xvfb "$DISPLAY" -nolisten unix >/dev/null 2>&1 &
sleep 0.5
exec node /opt/twatch/render.cjs "$@"
