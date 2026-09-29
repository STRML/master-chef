# Halo Vision 1.0

An unofficial, native Apple Vision Pro port of Halo: Combat Evolved for PC.
The project translates a user-supplied Halo PC executable into native code and
provides a Metal renderer, Windows compatibility layer, controller input,
audio, and an immersive visionOS presenter.

This is the **v1.0 source release**, based on the current runtime development
line. **You must supply your own Halo: Combat Evolved for PC retail disc or ISO
and a valid product key**, plus your own Apple signing setup for headset
installation. No game executable, maps, textures, sounds, product keys, saves,
or signed application are included.

## Features

- Native ARM64 execution with a Metal backend for the game's rendering API.
- A panoramic world with a stereo forward view and a separate game interface.
- Controller movement, aiming, menus, haptics, and reclined recentering.
- Local save storage, bounded diagnostics, and optional local texture packs.
- Hardware texture hashing and bounded reuse of static geometry.

## Get started

### Build quick start

Use an Apple Silicon Mac with Xcode, the visionOS SDK, command-line tools,
and Python 3.12. The app targets visionOS 26.0; the build was validated with
the 26.5 SDK.

Install your own Halo PC copy using your disc or ISO and valid product key,
then prepare the supported retail PC 1.10 installation. The app does not
load an ISO directly. Custom Edition, Anniversary, and other executables
are not interchangeable. Copy your matching executable to `game/halo.exe`
and check its SHA-256 against [the supported digest](docs/BUILDING.md#supply-the-executable).
Keep the complete installation for the game-data packaging step.

From the repository root:

```sh
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements-development.txt
shasum -a 256 game/halo.exe

python tools/generate_engine_reuse.py \
  @decompilation/c9acf0c46954/function-addresses.txt \
  @decompilation/c9acf0c46954/extra-function-entries.txt \
  --label whole-exe --max-functions 10000 --trap-unsupported --discover --chunks 32
python tools/export_engine_imports.py
python tools/build_engine_vision.py --configuration Release --direct --clean
```

This produces an unsigned app at
`native/EngineVision/.build/DirectXROS/HaloVision.app`. To run it on a Vision
Pro, follow [Building and installing](docs/BUILDING.md) to add your own game
files and private installation registry values, then sign and install with
your own Apple account. Keep product keys, registry files, game data, and
signed local packages out of public uploads.
[Controls](docs/CONTROLS.md) covers the in-game layout.

### Source checks

Source-only checks do not require game files:

```sh
python3 tools/run_source_checks.py --portable-only
```

On an Apple Silicon Mac, run `python3 tools/run_source_checks.py` for the
additional native, Metal, and Swift checks. Optional comparisons against the
original engine report a skip until locally generated code is available.

## Status

Playable sessions have been reported during development. Full-campaign
completion, consistent combat frame rates, panorama stitching, texture
stability, and uninterrupted audio are still being improved. The v1.0 label
identifies this source release; it is not a guarantee that every mission is
free of defects. See [known limitations](docs/KNOWN_ISSUES.md).

## Repository

| Path | Purpose |
| --- | --- |
| `native/EngineHost` | Metal, Windows API, audio, input, and panorama runtime |
| `native/EngineReuse` | Translated execution support and CPU semantics |
| `native/EngineVision` | visionOS app and presenter tests |
| `tools/engine_reuse` | Decoder/lifter adaptations |
| `tools` | Generation, build, local packaging, diagnostics, and checks |
| `third_party` | Required vendored dependencies and licenses |
| `decompilation` | Numeric entry-address lists for the supported executable |

## Licensing and attribution

The original project code is available under the [MIT License](LICENSE).
Third-party licenses and credits are preserved in
[THIRD_PARTY.md](THIRD_PARTY.md) and beside the vendored source.
Halo and its game assets belong to their respective owners. This project is
not affiliated with or endorsed by Microsoft, Bungie, Gearbox, or Apple.
