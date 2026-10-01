# Setting up the Quest 3 for the agent

This is the one-time headset + cable setup that lets the agent on this Mac
install, launch, and measure the Halo Quest 3 port. Everything below is
written against the actual state of this repo and this Mac (verified
2026-10-01). Once the headset shows up in `adb devices`, the agent can run
the whole on-device checklist in [docs/VALIDATION.md](../VALIDATION.md).

## 0. What the agent needs (the short version)

1. Developer mode on the headset (Meta account, one-time).
2. USB debugging switched on in the headset.
3. A **data** USB-C cable, headset unlocked, and the RSA trust prompt
   accepted on the headset.
4. Nothing else — the agent does the rest.

Verify success with:

```sh
~/Library/Android/sdk/platform-tools/adb devices
```

The row must read `<serial>  device` (not `unauthorized`, not `offline`,
and not an empty list).

## 1. One-time developer registration (headset side)

1. On the Mac, open <https://dashboard.meta.com> and sign in with the
   Meta account that owns the headset.
2. Create an organization when prompted (any name).
3. Register the headset by its serial number
   (headset **Settings → System → About → Serial Number**).
4. Meta changed the developer program in 2025: the free tier covers
   sideloading and USB debugging — which is all this project needs.
   The paid tier is only for Store publishing. The dashboard shows
   which tier the account is on; if the headset is already listed as a
   development device, skip this section.

## 2. Enable developer options on the headset

Put the headset on (or use the mirror window on the Mac):

1. **Settings → System → Developer**.
2. Toggle **USB Debugging** ON.
3. Optional: toggle **Wireless Debugging** ON (see section 6).

If the **Developer** submenu is missing, the account/headset pairing in
section 1 hasn't propagated yet — restart the headset once.

## 3. Plug in the cable

- Use a **USB-C data cable** (the brick that ships with the headset is
  charge-only and will produce an empty `adb devices` list).
- Plug directly into the Mac Studio, not through a hub that only
  provides power.
- The headset screen must be **on and unlocked** the first time: Android
  shows a key-trust dialog — **Allow USB debugging?** with the computer's
  RSA key fingerprint. Tick **Always allow from this computer**, then
  **Allow**. Without this tap the device stays `unauthorized` forever.

## 4. adb on this Mac

adb is installed but **not on PATH**. Use the full path, or add it once:

```sh
export PATH="$HOME/Library/Android/sdk/platform-tools:$PATH"
```

The project's own tooling already handles this:

- `tools/setup_halo.py --stage Quest` calls `adb` from PATH, so export
  the PATH line above (or pass `--adb ~/Library/Android/sdk/platform-tools/adb`)
  in the same shell.
- The device rows in `docs/VALIDATION.md` use the full path.

## 5. Confirm the link

```sh
~/Library/Android/sdk/platform-tools/adb devices -l
```

Expected:

```
List of devices attached
1W16xxxxxxxxx        device product:SEBT product:Quest 3 ...
```

If the state is wrong:

| State | Cause | Fix |
|---|---|---|
| empty list | charge-only cable, hub, or USB debugging off | section 3, section 2 |
| `unauthorized` | RSA prompt never accepted | unlock headset, re-plug, tap **Allow** |
| `offline` | stale adb server | `adb kill-server && adb start-server` |

## 6. Optional: wireless adb (no cable)

Headset **Settings → System → Developer → Wireless Debugging → Pair
devices with pairing code** shows an IP:port and a 6-digit code. Then:

```sh
~/Library/Android/sdk/platform-tools/adb pair 192.168.x.x:37000  # prompts for the 6-digit code
~/Library/Android/sdk/platform-tools/adb connect 192.168.x.x:37001
```

The headset must stay on the same Wi-Fi as the Mac. The agent will use
whichever transport is connected.

## 7. What the agent does after this

All of the following is automatic — the agent runs it; the headset just
needs to stay unlocked and connected:

```sh
# build native libs + APK, install, push the 1.4 GB game payload
python3 tools/setup_halo.py --stage Quest

# or, step by step:
make -C native/EngineHost/android -j 8 jniLibs
cd android && JAVA_HOME=/opt/homebrew/opt/openjdk@17 \
  ANDROID_HOME=$HOME/Library/Android/sdk \
  gradle --no-daemon -q assembleDebug
~/Library/Android/sdk/platform-tools/adb install -r \
  app/build/outputs/apk/debug/app-debug.apk
~/Library/Android/sdk/platform-tools/adb push game/. \
  /sdcard/Android/data/com.masterchef.haloquest/files/game
~/Library/Android/sdk/platform-tools/adb shell \
  am start -n com.masterchef.haloquest/android.app.NativeActivity
```

The app then appears in the headset library as **Halo Quest** (filter
*Unknown* in the app drawer if you want to put it on by hand).

The payload lands in the app's **external** files dir
(`/sdcard/Android/data/com.masterchef.haloquest/files/game`); the app
resolves it there and logs the resolved root on startup, so the agent
confirms the path with:

```sh
~/Library/Android/sdk/platform-tools/adb logcat -s haloquest
```

## 8. The five on-device rows the agent will run

From [docs/VALIDATION.md](../VALIDATION.md) — these are the rows that
keep the goal open until a headset is attached:

1. Main menu renders stereo (screencap + logcat).
2. First level 10 minutes crash-free (crash buffer + `dumpsys`).
3. 72 Hz in combat scenes (`dumpsys gfxinfo framestats`, display state).
4. Audio without underruns (`logcat AAudio AudioTrack`).
5. Controller move/aim/fire/menu (getevent + screencap diff).

## 9. Troubleshooting

| Symptom | Fix |
|---|---|
| `adb devices` empty | charge-only cable (most common), USB debugging off, or the headset is asleep — wake it by wearing it |
| `unauthorized` forever | unlock the headset, re-plug, accept the RSA dialog; `adb kill-server` first |
| `INSTALL_FAILED_UPDATE_INCOMPATIBLE` | an older build signed with a different key: `adb uninstall com.masterchef.haloquest` |
| app launches to black | `adb logcat -s haloquest` — the `game root:` line tells you the payload path the app actually used |
| app crashes at launch | `adb logcat -b crash` and the `haloquest` tag |
| `adb push` fails | check `adb shell df /sdcard` — the payload is 1.4 GB |
| `adb: no devices` right after a reboot | headset not unlocked yet; wake it |

## 10. Notes on this Mac

- adb: `~/Library/Android/sdk/platform-tools/adb` (37.0.1, not on PATH).
- NDK r27c: `tools/.cache/android-ndk-r27c` (gitignored).
- Gradle 9.8.0 + `openjdk@17` + SDK platform `android-34` — the
  `setup_halo.py --stage Quest` step wires all of it.
