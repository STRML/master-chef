# Changelog

## 1.0.1

- Adds `setup.sh` for guided retail ISO installation, existing-install imports,
  private registry conversion, dependency setup, generation and Xcode handoff.
- Bundles locally owned game files into the generated Xcode project and enables
  automatic signing with the user's account.
- Adds readiness reports, disk checks, resumable setup, and setup regressions.
- Adds a dedicated agent setup guide and Windows-prepared installation fallback.
- Keeps the original installer/key entry, patch selection and Apple signing as
  explicit human steps; no game files or keys are distributed.

## 1.0.0

First curated public source snapshot of the native visionOS runtime.
Original project code is released under the MIT License.

- Includes the current Metal renderer, panorama scheduler, controller and
  audio integration, ARM texture CRC path, and bounded geometry reuse.
- Includes source-only C, Objective-C, Metal, Swift, and Python checks.
- Moves the pinned decoder/lifter dependency to `third_party/xwa`.
- Parameterizes device and signing inputs and uses a generic emulated player.
- Replaces development handoffs with build, controls, architecture, and
  limitations documentation.
- Excludes historical Git data, prototype applications, local reports,
  generated engine code, game assets, private signing data, and binaries.

This version identifies the source distribution. See docs/KNOWN_ISSUES.md for
runtime limitations and docs/VALIDATION.md for the release checks actually run.
