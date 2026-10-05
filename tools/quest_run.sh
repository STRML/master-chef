#!/bin/sh
# quest_run.sh - one-command Halo Quest device run (Sam's "get home and press go").
#
# Builds jniLibs + APK, installs, launches Halo Quest as a VR-armed activity,
# then streams the haloquest log and grabs a screencap. The headset must be
# awake and a Touch controller recently active (Meta's launch check), or the
# app should already be set up in Kiosk mode (Device Access app on-headset).
#
# Usage: ./tools/quest_run.sh [--no-build]
set -e
cd "$(dirname "$0")/.."
ADB="$HOME/Library/Android/sdk/platform-tools/adb"
PKG=com.masterchef.haloquest
ACT=android.app.NativeActivity
LOG=.scratch/quest_run.log
CAP=.scratch/quest_frame.png

if [ "$1" != "--no-build" ]; then
    echo "== make jniLibs"
    make -C native/EngineHost/android jniLibs
    echo "== gradle assembleDebug"
    ( cd android && JAVA_HOME=/opt/homebrew/opt/openjdk@17 \
        ANDROID_HOME="$HOME/Library/Android/sdk" \
        gradle --no-daemon -q assembleDebug )
    echo "== install"
    "$ADB" install -r android/app/build/outputs/apk/debug/app-debug.apk
fi

"$ADB" shell "am force-stop $PKG" || true
"$ADB" logcat -c
"$ADB" shell input keyevent KEYCODE_WAKEUP || true
echo "== launch ($ACT, VR category; Meta launch check must pass)"
"$ADB" shell "am start -a android.intent.action.MAIN -c com.oculus.intent.category.VR -n $PKG/$ACT"

: > "$LOG"
for i in $(seq 1 40); do
    P="$("$ADB" shell pidof "$PKG" | tr -d '\r')"
    [ -n "$P" ] && break
    sleep 1
done
if [ -z "$P" ]; then
    echo "!! process never started - controller asleep? wake it (Oculus button) or use Kiosk mode."
    "$ADB" logcat -d -s haloquest "$ADB" 2>/dev/null | grep -iE "launchcheck|controller" | tail -3
    exit 1
fi
echo "pid=$P - streaming haloquest log for 30s"
sleep 30
"$ADB" logcat -d -s haloquest | tee "$LOG" | grep -vE "^---------" | head -60
"$ADB" exec-out screencap -p > "$CAP" 2>/dev/null && echo "screencap -> $CAP"
echo "full log: $LOG"
