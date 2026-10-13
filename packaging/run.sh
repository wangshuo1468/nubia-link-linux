#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
if [ "${XDG_SESSION_TYPE:-}" = "wayland" ]; then
    export QT_QPA_PLATFORM=xcb
fi
exec ./bin/nubia-link "$@"
