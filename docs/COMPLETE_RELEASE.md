# Complete release

Download **MasterChef-v1.0.3-Complete.zip** from the
[release page](https://github.com/mitchaiet/master-chef/releases/tag/v1.0.3).
Extract it on an Apple Silicon Mac. The archive includes this source tree,
the rebuilt unsigned `Release/HaloVision.app`, all campaign/multiplayer map,
shared texture, sound and shader data needed by this runtime, and the exact
visual packs used by Build91. It also includes notices and per-file hashes.

The replacement pack has 1,068 texture entries; this is the full installed
pack, not a claim that every original texture was repainted. The faithful
Pillar of Autumn corrections and held-original selections are retained.
Thirteen shader replacements and their CEnshine source/license are included.
[Runtime settings](../mods/runtime-settings.json) record the same startup
defaults. Tracking history, personal pose/calibration, saved games, registry
values and Apple signing material are excluded.

## Prepare your own installation

The app is **not signed for your headset**. Your own valid Halo PC product
registration and Apple signing are still required. Registration is imported
from your installed copy; never put your printed key in chat or a command.

If you have your own Halo registry export, run from the extracted folder:

```sh
./setup.sh --bundled --registry "/path/to/private/halo-install.reg"
```

An existing Wine installation can supply the registration instead:

```sh
./setup.sh --bundled --wine-prefix "/path/to/your/wine-prefix"
```

If you only have your original disc/ISO and printed key, run:

```sh
./setup.sh "/path/to/HALO.iso"
```

That route opens the original installer so you can enter your key there.
The [setup guide](SETUP.md) covers the retail PC 1.10 patch, Windows registry
export fallback and prerequisites. The Complete bundle supplies the visual
packs locally on either route. A source-only clone downloads the verified
visual-assets ZIP instead.

Setup validates/imports your registration, prepares a private game payload,
generates the native sources and opens Xcode. Select your Team and a unique
bundle identifier, select the unlocked Vision Pro, then press Run. This route
builds and signs a private app with the complete resources already connected.
Keep at least 12 GiB free beyond the extracted bundle and original ISO.

The precompiled app is included for inspection and advanced local signing;
it cannot launch correctly until your private registry and payload manifest
are added. The guided route above handles that through a fresh local build.
See [BUILDING.md](BUILDING.md) for explicit profile/device signing inputs.
Nothing in the archive grants a different Apple account permission to install
the owner's signed app; no owner profile or device identifier is shipped.

## Verify the download

Download the matching `.sha256` file beside the ZIP and run:

```sh
shasum -a 256 -c MasterChef-v1.0.3-Complete.zip.sha256
```

`RELEASE_MANIFEST.json` lists every file in the Complete archive except itself.
`SOURCE_MANIFEST.json` lists the source portion. `mods/visual-assets.json`
pins every visual pack plus its downloadable archive. The visual-assets ZIP
is also available separately; source setup verifies it before extraction.

## Scope and validation

The app uses source version 1.0.3/build 103. Its gameplay/runtime fixes are
the same as owner Build91; packaging and default resource inclusion changed.
Build91 was installed and startup-verified on a headset. Build103 is freshly
compiled and package-verified; it is not a new campaign qualification run.
Known combat FPS, panorama and audible-continuity limitations remain in
[KNOWN_ISSUES.md](KNOWN_ISSUES.md).

MIT applies to original project code. Original game content and third-party
mods retain their respective copyrights and notices; see
[ASSET_NOTICES.md](ASSET_NOTICES.md). Do not publish your configured private
app, registry exports, `.setup` directory, saves or Apple signing material.
