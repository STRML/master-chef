# Source-release validation

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
