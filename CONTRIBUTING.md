# Contributing

Keep changes focused and describe the visible behavior or correctness issue.
Run the source checks relevant to the change and include their results. For
rendering or performance changes, distinguish source tests, desktop Metal
checks, and actual headset gameplay measurements.

Keep game files, generated engine code, saves, credentials, provisioning
profiles, app packages, and raw diagnostic captures out of commits. Run
`python3 tools/check_repository_hygiene.py --strict` on a clean source tree
before preparing a distribution. Preserve third-party license notices.

Contributions to the original project code are submitted under the MIT
License. Third-party code and optional assets retain their separate licenses.
