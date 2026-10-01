# Validation

> The Quest 3 checklist below (issue #1, Phase 7) is the live completion
> artifact for the port. Captures referenced as `.scratch/validation/*` are
> real files in the working tree; their SHA-256 was computed with
> `shasum -a 256` on 2026-10-01. Device rows are marked ⚠ needs device:
> no Quest 3 was attached when recorded (`~/Library/Android/sdk/platform-tools/adb
> devices` → `List of devices attached`, rc=0; adb 37.0.1 present, not on
> PATH). Toolchain state as of 2026-10-01 (post-SdkBootstrap):
> SDK at `~/Library/Android/sdk` (platforms;android-34,
> build-tools;34.0.0, licenses accepted rc=0), gradle 9.8.0 at
> `/opt/homebrew/bin/gradle`, `apksigner` at
> `~/Library/Android/sdk/build-tools/34.0.0/apksigner` (needs
> `JAVA_HOME=/opt/homebrew/opt/openjdk@17`).

## Quest 3 on-device validation checklist (issue #1, Phase 7)

- [x] **CPU-only regression trace**
  - Command (host, arm64-native inside the lavapipe container — this is the
    trace gate; `qemu-aarch64` user-mode is **not** installed on this Mac,
    `which qemu-aarch64` rc=1):
    `docker run --rm --platform linux/arm64 -v "$PWD:/work" -w /work halo-vk-dev ./native/EngineHost/android/halo-headless-static ./game --frames 60`
  - Capture: text log → `.scratch/validation/cpu-trace-60f.log`
    sha256 `3dc45703a583a27ca3d1a00277de112e21a4b9451933872b582b8866f799376f`
    (2122 lines). Binary under test
    `native/EngineHost/android/halo-headless-static`
    sha256 `207df6c5a4b25eb6298cb69b96c0a3c4a4304e6c2786a98c1ac427165e27688c`.
  - Evidence: `[host] d3d9 Present: frame 60 (7434 draw calls)`
    (log line 2113), `[host] headless run finished rc=0` (line 2121),
    observed rc=0, 3.99 s wall (re-run by ValidationDoc; integrator run
    4.47 s).
  - Status: **PASS on host** (docker linux/arm64). On-device execution is
    ⚠ needs device; the host trace is the CPU regression gate.

- [ ] **Main menu renders stereo**
  - Command (device):
    `adb shell am start -n com.masterchef.haloquest/android.app.NativeActivity`
    then per-eye capture
    `adb exec-out screencap -p > .scratch/validation/menu-left.png`
    (right eye via headset view capture; log stream
    `adb logcat -s haloquest > .scratch/validation/menu.log`).
  - Capture: PNG (screencap) + logcat text, each sha256-recorded.
  - Status: ⚠ needs device. Headless proxy for the render surface is the
    Vulkan row below (lavapipe mono framebuffer, not stereo); stereo
    presentation requires the OpenXR shell on hardware (OpenXRShell lane).

- [ ] **First level 10 minutes no crash**
  - Command (device): start level from menu, then
    `adb logcat -b crash -T 0 > .scratch/validation/level10min-crash.log`
    for 600 s; crash-free iff the log is empty and
    `adb shell dumpsys activity processes | grep -c haloquest` stays ≥1.
  - Capture: logcat text + `dumpsys gfxinfo com.masterchef.haloquest`
    snapshot, each sha256-recorded.
  - Status: ⚠ needs device.

- [ ] **72Hz in combat scenes**
  - Command (device):
    `adb shell dumpsys gfxinfo com.masterchef.haloquest framestats > .scratch/validation/combat-framestats.txt`
    during a combat scene; pass iff the OpenXR app is running the 72 Hz
    refresh profile (`adb shell dumpsys display | grep -i 'mRefreshRate\|72'`)
    and gfxinfo frame times stay under 13.9 ms p95.
  - Capture: gfxinfo text + display-state text, sha256-recorded.
  - Status: ⚠ needs device. The lavapipe container is a software rasterizer
    and cannot evidence headset frame rate.

- [ ] **Audio without underruns**
  - Command (device):
    `adb logcat -s AudioTrack AAudio haloquest > .scratch/validation/audio.log`
    during gameplay; pass iff no `underrun`/`AudioTrack.*blocking` events
    and the port's own queue watchdog never rebuilds
    (`grep -c 'watchdog.*rebuilt' audio.log` == 0).
  - Capture: logcat text, sha256-recorded.
  - Status: ⚠ needs device. Host CPU-trace context: DirectSound stub logs
    `DirectSound output started: stereo float PCM at 48000 Hz` and two
    `[audio-watchdog] ... rebuilt the queue` rebuilds with
    `[audio-result] frames=0 nonzero=0` (expected on the stubbed CPU-only
    path — silent by design, not an audible-output test). AAudio sink work
    lives in the AaudioOutput lane.

- [ ] **Controller movement/aim/fire/menus**
  - Command (device): with Touch controllers connected,
    `adb shell getevent -l > .scratch/validation/controller-getevent.log`
    while exercising stick-move, aim, trigger-fire and menu select per
    `docs/CONTROLS.md`; pass iff each action produces the mapped
    `KEY_*/ABS_*` events **and** the game responds on screen (screencap
    diff). Input bridge log:
    `adb logcat -s haloquest > .scratch/validation/controller-game.log`.
  - Capture: getevent text + per-action PNG, sha256-recorded.
  - Status: ⚠ needs device. Host trace shows the guest input calls reach
    the shim (`dinput8.dll!IDirectInputDevice8A::GetDeviceState/GetDeviceData`
    in cpu-trace-60f.log) — wiring evidence only.

- [ ] **Hygiene check**
  - Command (host): `python3 tools/check_repository_hygiene.py`
  - Capture: text report → `.scratch/validation/hygiene.out`
    sha256 `39f40cbbb9b51c17f9a223092bb02de4198294a413dcb9f404830d433d3aea32`
    (4604 lines).
  - Status: **FAIL on the current working tree** — observed 2026-10-01:
    rc=1, `FAIL: 8936 source files checked; 4603 findings`. Breakdown:
    4590 findings under `tools/.cache/` (local NDK r27c cache +
    `ndk.zip`, untracked build cache), 4 under `native/` (staged headless
    binaries), 2 under `.scratch/` (lane work). The gate is a PASS on the
    release tree with caches excluded; resolving tracked-path findings is
    the QuestStage integrator's cutover. Row flips on a `PASS` capture.

- [ ] **Report distinguishes preparation / compilation / signing / install / launch / gameplay**
  - This section is that report; per-phase commands and current evidence:
    | Phase | Command | Evidence (observed 2026-10-01) | Status |
    | --- | --- | --- | --- |
    | Preparation | `python3 tools/setup_halo.py --stage Quest --dry-run` | rc=0; prints the 4 build/install steps, pushes game payload path `/sdcard/Android/data/com.masterchef.haloquest/files/game`; capture `.scratch/validation/quest_dry.out` sha256 `1481021fb8be9cf11277a380b71acbccf591d11fdd515c6caf6d08f0fbc0e515` | **PROVEN (dry-run, nothing executed)** |
    | Compilation | `make -C native/EngineHost/android -j 30 jniLibs` | `native/build/android-arm64/apk/jniLibs/arm64-v8a/libhaloquest.so` staged, sha256 `43a57d1dc80a96a774ef4d45b56caed334ec3f1bae40aead9bf295ecf72ceb82`; headless artifacts built (see rows above) | **DONE for native libs** |
    | Signing | `JAVA_HOME=/opt/homebrew/opt/openjdk@17 ~/Library/Android/sdk/build-tools/34.0.0/apksigner verify --print-certs <apk>` | **PROVEN** in sandbox `~/Library/Android/lanebuild`: gradle assembleDebug rc=0 produced `app-debug.apk` 14,599,490 B sha256 `44c4616b53d3cc6eaaabedf0f61c022200970bcfcf0445e9b7b9be61fa2a385d`; apksigner verify rc=0, Signer #1 `CN=Android Debug` cert sha256 `3ce0c4b0899c571fb403d738336ff13c1f3c2aadf5e4c109327aa186fae9992c`; capture `.scratch/validation/apk-verify.txt` sha256 recorded below. Repo build blocked until QuestStage fixes manifest `--`/`android:hwVulkan` + settings.gradle pluginManagement (use `gradle -I ~/Library/Android/init-agp.gradle` meanwhile) | **APK debug-signing PROVEN (sandbox); repo build blocked** |
    | Install | `adb install -r android/app/build/outputs/apk/debug/app-debug.apk` | adb 37.0.1 functional (`adb devices` rc=0) but `List of devices attached` empty — no Quest 3 plugged in | ⚠ needs device |
    | Launch | `adb shell am start -n com.masterchef.haloquest/android.app.NativeActivity` | no device attached | ⚠ needs device |
    | Gameplay | device rows above (stereo menu, 10-min level, 72 Hz, audio, controllers) | no device attached | ⚠ needs device |

### Headless Vulkan (lavapipe) — prerequisite gate for the device rows

- [x] **Vulkan headless 60 frames on lavapipe**
  - Command (host, docker):
    `docker run --rm --platform linux/arm64 -v "$PWD:/work" -w /work/native/build/vk-obj halo-vk-dev ./halo-headless-vk /work/game --frames 60`
  - Capture: text log → `.scratch/validation/vk-60f.log` sha256
    `b4fb07ca45bbc248490afbd757a8c8a31f5fe929365350aa5874d50c9472b3dc`.
    Binary under test `native/build/vk-obj/halo-headless-vk` sha256
    `09e5fcac3d93dd811f12a38994b2f836b51fd31f91856bdee0a6c8d12cbae26e`
    (built 2026-10-01 16:12).
  - Status: **PASS (2026-10-01)** — rc=0, 60 frames, 7198 engine draw
    calls, `VK_LAYER_KHRONOS_validation` enabled by the renderer: 0
    validation errors, 0 warnings, 0 crashes. The frame-2 segfault
    (`0x52CDB6`) was fixed by the `VK_NO_PROTOTYPES` correction (defining
    it to 0 still strips prototypes; the loader pointer was sign-extended
    on LP64) in `vulkanrenderer.c`; the `copy_to_image` queue-as-command-
    buffer bug was fixed in the same pass.

- [x] **Vulkan renderer pixel gates (lavapipe)**
  - Command (host, docker):
    `docker run --rm --platform linux/arm64 -v "$PWD:/w" -w /w halo-vk-dev`
    `bash -c 'gcc -O1 -g -std=gnu11 -w -I native/EngineHost native/EngineHost/android/vkdev/vk_pixel_test.c native/EngineHost/vulkanrenderer.c -o /tmp/vk_pixel_test -lvulkan -lm && VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json /tmp/vk_pixel_test'`
  - Source: `native/EngineHost/android/vkdev/vk_pixel_test.c`.
  - Status: **PASS (2026-10-01)** — 4/4 gates: clear→red across all 256 px;
    textured quad (white texel x green diffuse = green); alpha-test discard
    (128/255 < 200 -> blue clear survives); 16x16->32x32 GPU stretch blit
    fills all 1024 px including corner (31,31), proving the swapchain
    blit path. Zero validation errors under the renderer's own
    `VK_LAYER_KHRONOS_validation`.

## Legacy: 1.0.x visionOS source-release validation

The sections below are the historical record for the 1.0.0–1.0.3
visionOS/macOS source releases; they are not evidence for the Quest 3
checklist above.

## 1.0.3 complete packaging update

- Fresh public-source engine generation completed with 8,336 functions and
  32 chunks. An unsigned ARM64 visionOS Release build compiled 59 native units
  and 24 Swift sources, version 1.0.3/build 103, SDK 26.5.
- The direct app contains the exact Build91 texture and shader packs: 1,068
  texture entries and 13 shader replacements. All pack hashes match the
  installed baseline. The CEnshine source/license archive is also included.
- Xcode project generation confirms every visual pack and manifest in Copy
  Bundle Resources. Previously these resources were omitted by public builds.
- All nine visual-asset/content tests passed: exact import/resume/bundling, outer
  checksum rejection, changed-pack preservation, unsafe and symbolic entry
  rejection, invalid inner content, symbolic destination rejection and local
  Complete-bundle reuse without a network request, plus preservation of the
  original configuration and movie assets while excluding saves/registration.
  The 24 setup regressions
  and portable source suite (32 C targets plus additional checks) also passed.
- The renderer, audio recovery, native settings and settings UI sources match
  the frozen Build91 sources. The sampled headset preferences contained no
  saved resolution override; the recorded startup resolution was 2048x1536.
  Device tracking history and physical pose are not copied into the release.

The new build is package-verified and unsigned. Build91 installation and
startup evidence below remains the headset evidence for these runtime fixes;
it does not prove a new Build103 installation, full mission performance or
audible continuity. Other users supply their own registration and signing.

## 1.0.2 rendering and audio recovery update

Validated on an Apple Silicon Mac:

- Full source suite: 59 C checks plus Objective-C, Metal, Swift and Python
  checks passed. The portable subset also passed normally and with ASan/UBSan
  (32 C checks). Optional comparisons requiring generated engine code were
  skipped in the public checkout.
- Real-Metal regressions reproduced the previous sampler/depth cache failures
  and the rejected mixed texture address modes. The fixes passed 768 sampler
  requests and 192 depth/stencil states, including actual queued draws beyond
  both cache capacities. Cache storage remained bounded.
- All 25 U/V address combinations passed nine out-of-bounds pixel comparisons
  with each renderer path: 450 exact GPU pixel checks. Nonzero border colors
  remain unsupported.
- The production audio coordinator passed injected activation, rebuild and
  queue-start failures, two-second retry timing, background/interruption
  suppression, denied resume permission and stopped-runtime checks. The old
  coordinator reproduced the permanent-suspension failure. The new renderer
  and audio tests also passed focused ASan/UBSan runs. Audio framework calls
  in this lifecycle test are mocked; this is not an audible-output test.
- All 24 setup regressions and four documentation-art hygiene regressions
  passed. The reviewed header requires an exact path and SHA-256 match.

The corresponding owner Build91 was compiled from a frozen private snapshot
with fresh objects: 59 native units, 32 translated chunks and 24 Swift sources.
Signing and the complete local game-payload manifest passed. That bundle is
private. The public source app uses version 1.0.2/build 102, a separate build
number series, and requires the user's game files and signing.

Desktop entry probes for Halo (a30) and The Maw (d40) each completed 2,400 frames
with scripted controller turns and normal audio mixing enabled. All ten
captured GPU layers were reviewed for each scene. Median measured entry-scene
rates were 29.78 and 29.86 FPS respectively. These short desktop probes do not
measure firefights, full missions, audible output or headset performance, and
do not establish an improvement over an equivalent baseline.

The initial owner installation attempt could not reach the headset. A later
attempt on September 29, 2026 (Central time) installed and launched Build91;
device inventory independently confirmed version 1.0.2/build 91. Five startup
diagnostic snapshots matched the expected BuildID. The final snapshot reached
engine frame 1,226 with immersive presentation active, 2,708 completed GPU
frames and zero GPU failures. Its audio queue was running with nonzero samples,
zero enqueue failures and zero watchdog rebuilds. This verifies installation
and startup, not audible continuity, visual acceptance or campaign performance.
Known limitations remain in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

CI now targets macOS because the host fixtures depend on Mach APIs and Apple's
linker. The local passes above are not hosted CI results; previous hosted runs
were blocked by the account's billing status. Source publication and device
acceptance remain separate gates.

## 1.0.1 setup update

- Passed 24 synthetic setup regressions: Windows/Wine registry parsing,
  private-value error handling, owner-only files, supported-file import,
  rollback, resume, preservation of changed files, payload manifests,
  low-space refusal, installer refusal without a human terminal, and ISO
  cleanup after failure/cancellation.
- Passed the portable source suite on an Apple Silicon Mac, including the
  new setup regressions.
- Checked a real owned retail ISO with `--stage check --json`: the layout was
  recognized, the original executable was identified as needing the PC 1.10
  update, and the temporary read-only mount was detached afterward.
- Verified XcodeGen with a synthetic local payload: both payload resources
  appear in Copy Bundle Resources and automatic signing is enabled.
- The current validation Mac had less than the required 12 GiB free. The
  preflight correctly reported this. A fresh Wine installer/key-entry/update
  session and the complete ISO-to-headset path were **not** run for this
  update. The Windows fallback export instructions were not exercised on a
  Windows machine. No new signed build or headset acceptance is claimed.

The earlier hosted GitHub Actions attempts were blocked by the account's
billing status before tests could start. Local test results above are not
hosted Linux CI results. Original runtime code is unchanged by this setup
update; app metadata is now 1.0.1 (build 101).

## 1.0.0 runtime/source baseline

The following checks were completed from the curated source tree on an
Apple Silicon Mac. Local game data was used only for generation and optional
reference comparisons; no game data or resulting binaries are distributed.

| Check | Result |
| --- | --- |
| Full source suite | Passed: 59 C targets plus additional Objective-C, Metal, Swift, and Python checks |
| Portable subset with ASan/UBSan | Passed: 32 C targets plus additional comparisons |
| Save preservation after generic player-path cleanup | Passed: reuse, missing-file detection, save-tree preservation, failed-promotion rollback |
| Fresh engine generation | 8,336 functions in 32 translation units; no unresolved function frontier |
| Unsigned visionOS Release compile and link | Passed: 59 native units and 24 Swift sources, ARM64, SDK 26.5, deployment 26.0 |
| Public version metadata | 1.0.0, build 100; no personal bundle, team, or device ID |
| Xcode project generation | Passed |
| Required signing/device inputs | Missing inputs rejected; no personal defaults |
| Source path/content hygiene | Passed |
| Gitleaks 8.30.1 with decoding | No unreviewed findings |
| Source ZIP integrity | Every archived file checked against the included SHA-256 manifest |

Two Gitleaks false positives were reviewed in unchanged xxHash AVX512
arithmetic. The checked-in configuration suppresses only those exact
expressions in that specific file. Third-party license notices are retained.
The privacy audit includes personal home paths, device/signing identifiers,
private-network addresses, secret patterns, binary artifacts, and archive
entry names. No automatic scan can guarantee discovery of every possible
secret; this export also uses an explicit source selection and manual review.

The full suite ran before generated code was present and reported skips for
optional original-engine comparisons. The later sanitizer subset ran with
freshly generated code, including native light/shadow gather comparisons.
A real-map gather test was skipped because no map fixture was supplied. The
portable subset was executed on macOS; the included Ubuntu CI workflow has
not yet run on a hosted runner for this release.

The generator records 15 undecodable entries and 47,843 explicit instruction
trap annotations in the discovered executable regions. These counts do not
establish whether the corresponding paths are reachable in gameplay. Keep
checking the generation receipt when changing the supported executable or
translator; unresolved instructions fail explicitly at runtime.

The freshly generated application was compiled and linked, but this public
source revision was not signed, installed, or played on a headset. No claim
of full-campaign completion, stable combat FPS, or resolved visual/audio
issues follows from these checks. See KNOWN_ISSUES.md.

Generated sources, app products, intermediate objects, project files, raw
reports, device data and private Git history were removed from the release
tree after validation. SOURCE_MANIFEST.json lists the source archive contents
except for the manifest itself; the external checksum covers the whole ZIP.
