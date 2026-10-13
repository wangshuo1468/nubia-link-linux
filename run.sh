#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
# The verified KDE Wayland path uses Qt's XWayland backend for OpenGL.
if [ "${XDG_SESSION_TYPE:-}" = "wayland" ]; then
    export QT_QPA_PLATFORM=xcb
fi
exec ./build/nubia-link "$@"
