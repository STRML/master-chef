# Preparing a source release

Use this curated tree as the starting point for a new public repository.
Do not attach the old private repository's `.git` directory, history, remote
configuration, tags, or release artifacts. The public ZIP contains only the
listed source files and its SHA-256 manifest.

1. Preserve the MIT license for the original project code and the separate
   notices for all third-party dependencies.
2. Run the hygiene scan and applicable source checks from this tree.
3. Verify the build route with locally owned game files. Keep resulting
   generated sources, reports, signing inputs and packages local.
4. Update VERSION, app marketing/build versions, CHANGELOG.md and validation.
5. Export source only. Compare the ZIP entry list and each file digest against
   `SOURCE_MANIFEST.json`; reject unexpected entries and symbolic links.

Publication of this prepared snapshot is a separate step. It contains no
Git history, credentials, owner device provisioning, or game payload.
