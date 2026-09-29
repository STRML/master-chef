# Set up with a coding agent

Open a clone of `https://github.com/mitchaiet/master-chef` in your coding
agent and give it this prompt, replacing only the ISO path:

> Set up Halo Vision from my owned retail Halo PC ISO at `/path/to/HALO.iso`.
> Read AGENTS.md and docs/SETUP.md, then use setup.sh. Run the readiness check
> first and handle the available setup steps. Let me enter my own product key
> directly in the original installer and handle Apple sign-in, signing, trust,
> and headset unlock myself. Never ask me to paste the key into chat. Tell me
> exactly which stage is verified and what human step remains.

For an already installed copy, add the paths to your owned PC 1.10 game folder
and private Halo registry export instead of an ISO. Do not paste the export's
contents. This route also works when Wine cannot run the original installer.

## Agent procedure

1. Read `docs/SETUP.md` and check the checkout's actual remote. The public
   repository is **mitchaiet/master-chef**. Do not infer a publication target
   from the surrounding workspace name. Setup itself needs no GitHub write
   access and must not publish anything.
2. Run a read-only readiness check from the repo root:

   ```sh
   ./setup.sh "/path/to/HALO.iso" --stage check --json
   ```

   Or add `--game-dir` and `--registry`/`--wine-prefix` for an installed copy.
   Exit 2 means prerequisites or inputs need attention. Inspect the JSON;
   do not treat a parseable report as a successful setup.
3. Resolve prerequisites using the user's existing authorization. The doctor
   names missing tools and disk space. Do not bypass the disk guard, delete
   unrelated files, weaken platform requirements, or silently install a
   privileged system component. Xcode, Wine compatibility and Apple sign-in
   may need the user's UI interaction. Do not change global Xcode selection
   or the user's normal Wine prefix as a shortcut.
4. For ISO installation, have the user run the command in an interactive
   Terminal so they can use the original installer and file picker. A remote
   agent without an interactive terminal should give that one command, then
   continue independent checks. `--non-interactive` intentionally refuses to
   launch the installer. The user supplies the retail PC 1.10 patch when
   prompted; do not substitute a Custom Edition binary or bypass its hash.
5. After the original installer has completed, the agent can resume without
   dialogs using the managed prefix:

   ```sh
   ./setup.sh --wine-prefix "$PWD/.setup/wine-prefix" --non-interactive --no-open
   ```

   For Windows-prepared inputs:

   ```sh
   ./setup.sh --game-dir "/path/to/owned/Halo" \
     --registry "/path/to/private/halo-install.reg" --non-interactive --no-open
   ```

   If `game/` is already prepared, use `./setup.sh --non-interactive --no-open`.
   The setup process validates copied files and imports only Halo's registry
   values. Do not inspect, print or attach raw product values. Never invent
   PID/DigitalProductID values or obtain somebody else's key.
6. Open `native/EngineVision/EngineVision.xcodeproj` when ready. The user
   selects their Team, unique bundle ID and paired Vision Pro, then presses
   Run. The game payload is already part of the project. Preserve signing
   changes on reruns. For explicit scripted signing, follow `docs/BUILDING.md`
   using only the user's supplied inputs; do not guess team or device IDs.
7. Verify each completed stage from actual outputs. `.setup/status.json`
   records the last setup stage; it does not prove device installation.
   Check Xcode's build/install result and the headset before claiming launch.
   A menu launch is not campaign, FPS, panorama or audio acceptance.

## Report back

Use a short checklist with evidence and unresolved steps:

- Prerequisites and supported executable: checked or blocked, with reason.
- Game data/registry import: validated, without revealing private values.
- Generated engine/Xcode project/unsigned build: which actually completed.
- Apple signing, installation and launch: separately confirmed or awaiting
  the user's action.
- Gameplay checks: only what was actually played or measured.

The setup regression command is `python3 tools/test_setup_halo.py`. It uses
synthetic fixtures; passing it does not prove a fresh Wine installer works
or that a headset can run the resulting app. Use `docs/VALIDATION.md` for the
release's specific validation boundaries.

Do not publish `game/`, `.setup/`, `.venv/`, registry exports, private logs,
generated engine source, signed apps or provisioning material. Before an
explicitly authorized source publication, inspect `git diff --cached` and
run the hygiene scanner against a clean source export, not a private game
installation. Never include an installation directory in a public ZIP.
