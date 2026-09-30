# Preparing a source release

Publish source updates to `mitchaiet/master-chef`, using this curated public
tree. Verify the remote before pushing. Keep private development history,
device identifiers, reports, game payload and signing artifacts separate.
The source ZIP includes the listed source/documentation files, reviewed
generated header art and a SHA-256 manifest; it contains no Git history.

1. Preserve the MIT license for the original project code and the separate
   notices for all third-party dependencies.
2. Run `python3 tools/run_source_checks.py` on an Apple Silicon Mac and the
   relevant sanitizer checks. The CI subset runs on macOS because some host
   fixtures require Mach APIs and Apple's linker. Report hosted CI separately.
3. Verify the build route with locally owned game files. Keep resulting
   generated sources, reports, signing inputs and packages local.
4. Update VERSION, app marketing/build versions, CHANGELOG.md and validation.
   The source release's app build number and the owner's private development
   build number are separate series. Identify both when comparing results.
5. Export source only. Compare the ZIP entry list and each file digest against
   `SOURCE_MANIFEST.json`; reject unexpected entries and symbolic links. The
   manifest lists every exported file except itself. Audit the clean export
   with `python3 tools/check_repository_hygiene.py --strict --root <export>`
   and scan the export/archive and outgoing Git history for secrets.

The header is permitted only at its reviewed path and SHA-256 in the hygiene
checker. Updating the artwork requires updating that digest after review;
never add a general binary or image exception.

## Complete release assets

Starting with 1.0.3, explicitly authorized Complete releases are separate
GitHub Release assets. Keep large files out of Git. Select static game assets
explicitly; exclude registry values, personal profiles, saves, logs, machine
paths, device identifiers, provisioning and signing keys. Build a fresh generic
unsigned app from public source, never distribute the owner's signed bundle.

Include the exact texture/shader packs, their provenance/notices and shader
source/license. Record all resource hashes and runtime defaults. Verify direct
build and Xcode resource inclusion; test checksum rejection, safe extraction,
existing-file preservation and offline bundle setup. `RELEASE_MANIFEST.json`
must describe every complete-archive entry except itself, including the source
manifest. Scan the binary, nested source archives and all selected game assets
for private values without printing those values. Recheck the public download
against the local ZIP and its per-file manifest after uploading.

Other users must provide their own valid registration and Apple signing.
Do not fabricate registration values to make a public package launch without
them. The included unsigned app and verified assets do not establish a signed
installation or a full gameplay acceptance run.

After publishing, verify the remote commit/tag and anonymously download the
release ZIP to compare its checksum. Keep signed device installs, launch,
startup telemetry, gameplay acceptance and source publication as distinct
validation steps. Do not upload the owner's complete app bundle.
