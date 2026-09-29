# Changelog

## 1.0.2

- Supports independent U/V texture addressing, mirrored repetition and mirrored
  clamping instead of rejecting those draws.
- Reuses bounded sampler and depth/stencil caches under pressure instead of
  permanently refusing new material states. Inactive stencil settings share
  one cache entry; failed state allocations remain retryable.
- Retries failed foreground audio activation, queue rebuilds and queue starts
  every two seconds. Recovery respects backgrounding, interruptions, denied
  resume permission and stopped runtime state.
- Adds real-Metal pixel and cache-pressure regressions plus deterministic audio
  lifecycle failure tests.
- Adds the generated Master Chef header, refreshed release documentation and
  a checksum-restricted documentation artwork exception in the hygiene audit.
- Runs source CI on macOS, matching the Mach APIs and Apple linker used by
  host fixtures. Probe freshness checks now include implementation includes.

These fixes are shared with the owner Build91. They do not establish stable
FPS, uninterrupted audible playback or a complete campaign playthrough.

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
