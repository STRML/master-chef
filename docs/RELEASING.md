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

After publishing, verify the remote commit/tag and anonymously download the
release ZIP to compare its checksum. Keep signed device installs, launch,
startup telemetry, gameplay acceptance and source publication as distinct
validation steps. Do not upload the owner's complete app bundle.
