#!/bin/bash
# Remote-launch Halo Quest through vrshell itself so the launch-id
# handshake succeeds without a human tapping the headset tile.
#
# vrshell (the home app) listens for its own debug broadcast; if that is
# locked on retail, fall back to input injection: MENU opens the universal
# menu, then taps on the stereo-mirrored launcher grid. Coordinates are
# tuned once via screencap and stored in .scratch/quest_tile_xy.txt.
#
# Usage: ./tools/quest_launch.sh [wake]   ("wake" also sends KEYCODE_WAKEUP)
set -uo pipefail
ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
PKG=com.masterchef.haloquest
ACT=com.oculus.nativeglue.OculusNativeActivity
XY=".scratch/quest_tile_xy.txt"

[ "${1:-}" = wake ] && "$ADB" shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1
"$ADB" shell svc power stayon true >/dev/null 2>&1
sleep 1

# 1) documented vrshell debug entry (usually disabled on retail)
if "$ADB" shell am broadcast -a com.oculus.intent.action.LAUNCH --es package_name "$PKG" \
     >/dev/null 2>&1 && sleep 6 && "$ADB" shell pidof "$PKG" >/dev/null 2>&1; then
    echo "launched via vrshell broadcast"; exit 0
fi

# 2) input-injection path: open universal menu, tap the saved tile point
if [ ! -s "$XY" ]; then
    echo "no tile coordinates yet: open the launcher once, run"
    echo "  $ADB exec-out screencap -p > .scratch/home.png"
    echo "and write 'X Y' (in the 2064x2208 panel space) to $XY"
    exit 2
fi
read -r TX TY < "$XY"
"$ADB" shell input keyevent KEYCODE_MENU >/dev/null 2>&1
sleep 2
"$ADB" shell input tap "$TX" "$TY" >/dev/null 2>&1
for _ in 1 2 3 4 5 6 7 8; do
    sleep 2
    if "$ADB" shell pidof "$PKG" >/dev/null 2>&1; then
        echo "launched via injected tile tap (pid $("$ADB" shell pidof "$PKG" | tr -d '\r'))"
        exit 0
    fi
done
echo "not running after tap - check screencap"
exit 1
