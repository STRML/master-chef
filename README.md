# Halo Vision 1.0.1

An unofficial, native Apple Vision Pro port of Halo: Combat Evolved for PC.
The project translates a user-supplied Halo PC executable into native code and
provides a Metal renderer, Windows compatibility layer, controller input,
audio, and an immersive visionOS presenter.

This is the **v1.0.1 source release**, based on the current runtime development
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

### One-command guided setup

On an Apple Silicon Mac with Xcode and the visionOS SDK:

```sh
git clone https://github.com/mitchaiet/master-chef.git
cd master-chef
./setup.sh "/path/to/HALO.iso"
```

The wizard mounts your ISO read-only, opens the original installer for your
key, applies your selected retail PC 1.10 updater, imports your game and
private installation values, generates the engine, and opens Xcode with
the game files already included. In Xcode, select your Apple Team, set a
unique bundle ID, choose your Vision Pro and press Run.

This is guided: you complete the original installer/update and Apple's
signing/trust prompts. It requires Python 3.12, XcodeGen, a compatible Wine
installation and 12 GiB free space. The [setup guide](docs/SETUP.md) covers
prerequisites, the patch, resuming, and a Windows fallback if Wine cannot run
the installer. An existing owned PC 1.10 installation can be imported without
Wine. Custom Edition, Anniversary, MCC, and Xbox discs are not supported.

- [Guided setup and troubleshooting](docs/SETUP.md)
- **[Set up with a coding agent — copy/paste prompt and dedicated instructions](docs/AGENT_SETUP.md)**
- [Manual build, signing, and installation](docs/BUILDING.md)
- [Controller layout](docs/CONTROLS.md)

Check readiness without installing or building:

```sh
./setup.sh "/path/to/HALO.iso" --stage check
```

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
