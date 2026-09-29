# Guided setup

Run this on an Apple Silicon Mac. You supply your own **retail Halo: Combat
Evolved for PC disc/ISO and valid product key**. The wizard prepares a private
app with your game files; this repository and its public downloads contain
source code only.

```sh
git clone https://github.com/mitchaiet/master-chef.git
cd master-chef
./setup.sh "/path/to/HALO.iso"
```

You can also run `./setup.sh` with no arguments to select the ISO in a file
picker. To check the Mac and ISO without installing anything:

```sh
./setup.sh "/path/to/HALO.iso" --stage check
```

## What you need once

- An Apple Silicon Mac, Xcode with the visionOS platform/SDK, and at least
  **12 GiB free space**, in addition to the ISO. The app targets visionOS 26.0;
  SDK 26.5 was used for the source-release build.
- Python 3.12 and XcodeGen. If you use Homebrew:
  `brew install python@3.12 xcodegen`. Setup installs the pinned Python
  dependencies into this checkout's `.venv` automatically.
- A macOS Wine distribution that can run 32-bit Windows installers, plus
  Rosetta when that distribution requires it. See the
  [WineHQ macOS installation page](https://wiki.winehq.org/MacOS).
  The [Homebrew wine-stable cask](https://formulae.brew.sh/cask/wine-stable)
  was disabled when this guide was prepared; do not assume the old Homebrew
  command still installs it. Existing Wine installations are detected; use
  `--wine "/path/to/wine"` for another location. The Windows fallback below
  avoids needing Wine on the Mac.
- The **retail PC 1.10 updater**, `halopc-patch-1.0.10.exe`, if your disc has
  the original executable. Have your own local copy ready; the wizard asks
  you to select it. [Bungie's original update announcement](https://www.bungie.net/en/Forums/Post/64943622)
  identifies the PC and Custom Edition patches separately. Historical download
  links may no longer resolve. This tool does not fetch arbitrary replacement
  executables or redistribute the patch. An already updated installation is
  also supported.
- Your Apple account in Xcode and a paired Vision Pro with Developer Mode
  enabled. Pair a game controller for playing.

Custom Edition, Anniversary, MCC, Xbox discs, and modified executables are
not supported by this translator. The installed executable must match the
[documented PC 1.10 SHA-256](BUILDING.md#supply-the-executable).

## What the wizard does

1. Checks the platform, build tools and free disk space. Mounts your ISO
   read-only and recognizes the original retail disc layout.
2. Opens the disc's original installer in a dedicated `.setup/wine-prefix`.
   **Enter your own key in the original installer window**, accept its terms
   yourself, keep the default installation path, and finish. Skip optional
   DirectX/GameSpy extras and do not launch the Windows game.
3. Opens your selected PC 1.10 update when needed, then checks the resulting
   executable's exact hash. You can supply the file up front:
   `./setup.sh "/path/to/HALO.iso" --patch "/path/to/halopc-patch-1.0.10.exe"`.
4. Imports only the executable, strings, maps, shader binaries and Halo
   installation registry values. It verifies all ten campaign maps and makes
   a local payload manifest. Your ISO and any existing installation remain
   intact. Your normal Wine prefix is not used or modified.
5. Creates the Python environment, generates the native engine, bundles your
   game files for Xcode, and opens the project. These steps can take time;
   progress names and private log locations are printed.

The original installer and patch still have their own windows. This is a
**guided setup**, not an unattended installer. Wine compatibility varies;
if an installer window never appears or installation fails, use the Windows
fallback rather than retrying indefinitely. Press Ctrl-C to cancel.

## Finish in Xcode

The generated project already includes your game payload and enables
automatic signing. No hand-edited registry template or separate packaging
command is needed on the guided path.

1. Add your Apple account under **Xcode → Settings → Accounts** if necessary.
2. Select the **EngineVision target → Signing & Capabilities**. Select your
   Team and replace `org.example.halovision` with a unique bundle identifier
   you control. Keep **Automatically manage signing** enabled.
3. Select your paired Vision Pro as the run destination. Unlock it and keep
   it awake; complete trust/Developer Mode prompts on the headset yourself.
4. Press **Run**. Xcode builds, signs, installs, and launches your private app.

An account/profile restriction or provisioning error must be resolved in
Xcode. Setup does not sign in for you, create certificates, or claim the app
is installed merely because project generation succeeded. For advanced
explicit signing and diagnostics, use [BUILDING.md](BUILDING.md).

## Already installed / Windows fallback

If you already have your owned, supported PC 1.10 installation, skip the ISO
installer entirely:

```sh
./setup.sh --game-dir "/path/to/your/Halo" --registry "/path/to/halo-install.reg"
```

`--registry` accepts a Windows Registry Editor export or an existing
`halo-vision-registry.txt`. An existing Wine prefix can supply the registry:

```sh
./setup.sh --game-dir "/path/to/your/Halo" --wine-prefix "/path/to/your/wine-prefix"
```

The supplied game folder and Wine prefix are read-only inputs. If Halo is
in the Wine default installation directory, `--wine-prefix` alone is enough.
Only Halo values are imported; unrelated registry sections are discarded.

To prepare the inputs on Windows:

1. Install your own retail disc/ISO, entering your valid key in the installer.
   Apply the retail PC 1.10 update.
2. In Command Prompt on **64-bit Windows**, export only Halo's installation
   key (choose a private output location):

   ```bat
   reg export "HKLM\SOFTWARE\WOW6432Node\Microsoft\Microsoft Games\Halo" "%USERPROFILE%\Desktop\halo-install.reg"
   ```

   On **32-bit Windows**, omit `WOW6432Node` from that registry path.
3. Copy the installed Halo directory and that private export to your Mac.
   Run the `--game-dir` command above. Setup checks the executable and game
   files before accepting them.

The registry export contains private product information. Do not post it,
its contents, or your printed key in chat, screenshots, issues, or releases.
The key is not interchangeable with the encoded `DigitalProductID` value.

## Resume and troubleshooting

Rerun the same command after fixing a prerequisite or completing a human
step. A valid `game/` is reused. Generated sources are reused when their
inputs match. Existing Xcode signing choices are preserved. Another setup
process cannot write to the same checkout at the same time.

| Command / option | Result |
| --- | --- |
| `./setup.sh --stage check --json` | Readiness report for a person or agent; no install/build |
| `./setup.sh … --stage prepare` | Import and validate owned game data only |
| `./setup.sh … --no-open` | Prepare the Xcode project without opening it |
| `./setup.sh … --stage build` | Build an unsigned app including your private game payload |
| `./setup.sh … --non-interactive` | Never open installer dialogs or prompt; stop with actionable missing inputs |
| `./setup.sh --help` | All options |

`…` above means your normal ISO or installed-game arguments, not a literal
argument. `--stage check` reports exit code 2 when action is needed; other
stages also use 2 for a setup failure. The wizard never asks for a key on the
command line. Agent setup instructions are in [AGENT_SETUP.md](AGENT_SETUP.md).

Local files are ignored by Git: `.setup/`, `.venv/`, `game/`, generated engine
sources, Xcode projects, and app build products. `.setup/` is owner-only;
registry exports and setup logs are written with owner-only permissions.
Copy-on-write is used where APFS supports it. No automatic disk cleanup runs.
Use a fresh checkout to import a different installation; existing game or
modified payload directories are deliberately preserved.

If you have an older generated Xcode project, preserve your Team/bundle ID
choices, then regenerate it after source generation:
`python tools/build_engine_vision.py --generate-only` from the activated venv.

Setup logs can contain local paths and installer output. Review them locally
and report the failed stage plus a redacted error, not entire raw logs.
Successful setup does not establish stable FPS, working audio, or completed
campaign gameplay. See [known issues](KNOWN_ISSUES.md) and
[what was actually validated](VALIDATION.md).
