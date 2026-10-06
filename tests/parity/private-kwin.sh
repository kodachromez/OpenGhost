#!/bin/sh
# Runs a test-enabled openghost-cpp on a private, headless KWin (Wayland, the
# GPU's own EGL), never the desktop: `kwin_wayland --virtual` with its own
# D-Bus session, runtime, config and data directories and no parent display.
#
# usage: private-kwin.sh <width> <height> <scale> <output-dir> <app args...>
#   e.g. private-kwin.sh 3840 2160 1.45 out --splash-check
#        MAXIMIZE=1 private-kwin.sh 3840 2160 1.45 out --splash-frames out --splash-at 600,1500
# The app runs maximized with MAXIMIZE=1; its output goes to <output-dir>/app.log.
set -eu
W=$1; H=$2; SCALE=$3; OUT=$(realpath -m "$4"); shift 4
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
APP=${APP:-$ROOT/build-release/openghost-cpp}
mkdir -p "$OUT"
# A Wayland socket path must stay under 108 bytes: a short runtime directory.
RUNTIME=$(mktemp -d /tmp/og-kwin.XXXXXX)
HOME_DIR=$(mktemp -d "${TMPDIR:-/tmp}/og-kwin-home.XXXXXX")
mkdir -p "$HOME_DIR/config" "$HOME_DIR/data" "$HOME_DIR/cache"
# The output's mode and fractional scale, as KWin reads them at start.
cat > "$HOME_DIR/config/kwinoutputconfig.json" <<EOF
[{"name": "outputs", "data": [{"connectorName": "Virtual-0", "scale": $SCALE,
  "mode": {"width": $W, "height": $H, "refreshRate": 60000, "flags": 1}}]},
 {"name": "setups", "data": [{"lidClosed": false, "outputs": [{"enabled": true, "outputIndex": 0,
  "position": {"x": 0, "y": 0}, "priority": 0}]}]}]
EOF
if [ "${MAXIMIZE:-0}" = 1 ]; then
    printf '[General]\ncount=1\nrules=1\n\n[1]\nDescription=maximize\nmaximizehoriz=true\nmaximizehorizrule=3\nmaximizevert=true\nmaximizevertrule=3\nwmclass=openghost\nwmclassmatch=2\n' \
        > "$HOME_DIR/config/kwinrulesrc"
fi
{
    echo '#!/bin/sh'
    echo 'export QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY=openghost-private-1 QT_QPA_PLATFORMTHEME=generic QT_FORCE_STDERR_LOGGING=1'
    printf '"%s"' "$APP"
    for arg in "$@"; do printf ' "%s"' "$arg"; done
    printf ' > "%s/app.log" 2>&1\n' "$OUT"
    printf 'echo "exit $?" >> "%s/app.log"\n' "$OUT"
} > "$HOME_DIR/app.sh"
chmod +x "$HOME_DIR/app.sh"
env -i HOME="$HOME_DIR" PATH=/usr/bin:/bin LANG=C.UTF-8 XDG_RUNTIME_DIR="$RUNTIME" \
    XDG_CONFIG_HOME="$HOME_DIR/config" XDG_DATA_HOME="$HOME_DIR/data" XDG_CACHE_HOME="$HOME_DIR/cache" \
    dbus-run-session -- kwin_wayland --virtual --width "$W" --height "$H" \
    --socket openghost-private-1 --exit-with-session "$HOME_DIR/app.sh" > "$OUT/kwin.log" 2>&1
tail -1 "$OUT/app.log"
grep -q '^exit 0$' "$OUT/app.log"
