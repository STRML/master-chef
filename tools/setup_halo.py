#!/usr/bin/env python3
"""Guided local setup from owned retail media or an existing PC 1.10 install."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import signal
import subprocess
import sys
import tempfile

from halo_setup_registry import RegistryError, read_registry
from prepare_engine_vision_device import (HALO_SHA256, clone_copy, PreparationError,
                                         manifest_rows, sha256_file, validate_game,
                                         validate_bundled_game_payload, REQUIRED_GAME_FILES)

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / '.setup'
GAME = ROOT / 'game'
GEN = ROOT / 'native/build/engine-reuse/whole-exe'
PROJECT = ROOT / 'native/EngineVision/EngineVision.xcodeproj'
GIB = 1024 ** 3
CAMPAIGN = ('a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40')
PATCH_INFO = 'https://www.bungie.net/en/Forums/Post/64943622'
QUEST_PACKAGE = 'com.masterchef.haloquest'
QUEST_DEVICE_GAME = '/sdcard/Android/data/com.masterchef.haloquest/files/game'
QUEST_ANDROID = ROOT / 'android'
QUEST_APK = QUEST_ANDROID / 'app/build/outputs/apk/debug/app-debug.apk'
QUEST_JNILIBS = ROOT / 'native/build/android-arm64/apk/jniLibs/arm64-v8a'


class SetupError(RuntimeError):
    pass


def private_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as f:
        temporary = Path(f.name)
        f.write(data.encode() if isinstance(data, str) else data)
    try:
        temporary.chmod(0o600)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def need_space(gib=12):
    free = shutil.disk_usage(ROOT).free / GIB
    if free < gib:
        raise SetupError(f'Free disk space is {free:.1f} GiB; setup needs at least {gib} GiB. '
                         'Free space and rerun the same command. No existing data was deleted.')


def find_wine(explicit=None):
    if explicit:
        path = Path(explicit).expanduser()
        return str(path.resolve()) if path.is_file() else shutil.which(explicit)
    candidates = [shutil.which('wine'),
                  '/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine',
                  '/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine']
    return next((str(Path(p).resolve()) for p in candidates if p and Path(p).is_file()), None)


def child_named(root, name):
    matches = [p for p in root.iterdir() if p.name.casefold() == name.casefold()]
    if len(matches) != 1 or matches[0].is_symlink():
        raise SetupError(f'Expected exactly one regular {name} in the selected installation/media')
    return matches[0]


@contextmanager
def mounted_iso(iso):
    iso = iso.expanduser().resolve(strict=True)
    if not iso.is_file() or iso.suffix.lower() != '.iso':
        raise SetupError('Choose a retail Halo PC .iso file')
    with tempfile.TemporaryDirectory(prefix='halo-iso-') as temp:
        mount = Path(temp) / 'disc'
        mount.mkdir()
        attached = False
        try:
            result = subprocess.run(['hdiutil', 'attach', '-readonly', '-nobrowse', '-plist',
                                     '-mountpoint', str(mount), str(iso)],
                                    capture_output=True, timeout=120)
            if result.returncode:
                raise SetupError('Could not mount the ISO read-only. Check that it is a valid disk image.')
            attached = True
            info = plistlib.loads(result.stdout)
            points = [Path(e['mount-point']).resolve() for e in info.get('system-entities', [])
                      if 'mount-point' in e]
            if mount.resolve() not in points:
                raise SetupError('Disk image did not mount at the requested temporary location')
            yield mount
        finally:
            # Detach only this invocation's mount, never a user's existing disc.
            if attached or os.path.ismount(mount):
                result = subprocess.run(['hdiutil', 'detach', str(mount)], capture_output=True, timeout=30)
                if result.returncode:
                    # Do not recurse into a still-mounted disk during temp cleanup.
                    subprocess.run(['hdiutil', 'detach', '-force', str(mount)],
                                   capture_output=True, timeout=30, check=True)


def inspect_disc(mount):
    setup = child_named(mount, 'Setup.exe')
    exe = child_named(child_named(mount, 'Files'), 'halo.exe')
    cabinets = child_named(mount, 'FilesCab')
    if not setup.is_file() or not exe.is_file() or not cabinets.is_dir():
        raise SetupError('This is not the supported retail Halo PC disc layout')
    if not list(cabinets.glob('*.[Cc][Aa][Bb]')):
        raise SetupError('Retail disc cabinet files are missing')
    return {'retailLayout': True, 'executableSHA256': sha256_file(exe),
            'needsPC110Update': sha256_file(exe) != HALO_SHA256}


def choose_file(prompt, non_interactive):
    if non_interactive or not sys.stdin.isatty():
        raise SetupError(prompt + ' Supply the corresponding path option, or rerun interactively.')
    # A normal file picker avoids shell escaping and putting product keys in a terminal.
    result = subprocess.run(['osascript', '-e',
                             'on run argv\nPOSIX path of (choose file with prompt (item 1 of argv))\nend run',
                             prompt], capture_output=True, text=True)
    if result.returncode:
        raise SetupError('File selection cancelled; rerun the same command to resume')
    return Path(result.stdout.strip()).resolve(strict=True)


def run_logged(command, label, env=None, cwd=ROOT, timeout=None):
    logs = LOCAL / 'logs'
    logs.mkdir(parents=True, exist_ok=True, mode=0o700)
    log_path = logs / (label + '.log')
    print(f'{label}… (private log: .setup/logs/{label}.log)', flush=True)
    fd = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as log:
        process = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env,
                                   stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except (KeyboardInterrupt, subprocess.TimeoutExpired):
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise SetupError(f'{label} stopped. Private files are retained; rerun to resume.') from None
    if code:
        raise SetupError(f'{label} failed (exit {code}). Review its private local log; do not upload it unredacted.')


def prefix_game(prefix):
    candidates = [prefix / 'drive_c' / program / 'Microsoft Games' / 'Halo'
                  for program in ('Program Files', 'Program Files (x86)')]
    matches = [p for p in candidates if p.is_dir() and any(f.name.lower() == 'halo.exe' for f in p.iterdir())]
    if len(matches) > 1:
        raise SetupError('More than one Halo installation found. Select one with --game-dir.')
    return matches[0] if matches else None


def iso_install(args):
    wine = find_wine(args.wine)
    if not wine:
        raise SetupError('A macOS Wine installation is needed for the original Windows installer. '
                         'See docs/SETUP.md, or use --game-dir with an existing PC 1.10 installation.')
    if args.non_interactive or not sys.stdin.isatty():
        raise SetupError('The original installer needs a human to enter their key. Run setup in Terminal, '
                         'or supply --game-dir and --registry/--wine-prefix. No installer was started.')
    prefix = LOCAL / 'wine-prefix'
    prefix.mkdir(exist_ok=True, mode=0o700)
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG='-all')
    # Never inherit a global Wine architecture choice or mutate ~/.wine.
    env.pop('WINEARCH', None)
    iso = args.iso or choose_file('Choose your Halo PC retail ISO', False)
    with mounted_iso(iso) as mount:
        inspect_disc(mount)
        game = prefix_game(prefix)
        if game is None:
            print('The original Halo installer will open. Enter your own key in that window,\n'
                  'accept its terms yourself, keep the default installation location, and finish setup.\n'
                  'Do not launch Halo or install DirectX/GameSpy extras. Close the installer to continue.', flush=True)
            try:
                run_logged([wine, 'start', '/wait', '/unix', child_named(mount, 'Setup.exe')],
                           'retail-installer', env, mount, 45 * 60)
            finally:
                # Flush this dedicated prefix's registry and release the mounted disc.
                server = Path(wine).with_name('wineserver')
                if server.is_file():
                    subprocess.run([str(server), '-k'], env=env, capture_output=True, timeout=30)
            game = prefix_game(prefix)
        if game is None:
            raise SetupError('Installer did not create a Halo installation. See the Windows fallback in '
                             'docs/SETUP.md; Wine installer compatibility varies. No successful install is claimed.')
        if sha256_file(child_named(game, 'halo.exe')) != HALO_SHA256:
            print('PC 1.10 update required. Use the retail PC patch (not Custom Edition).\n'
                  f'Original publisher announcement: {PATCH_INFO}', flush=True)
            patch = args.patch or choose_file('Choose your retail halopc-patch-1.0.10.exe', False)
            if not patch.is_file() or patch.suffix.lower() != '.exe':
                raise SetupError('The patch must be a local Windows .exe installer')
            print('The selected update installer will open. Complete it and close its window.', flush=True)
            try:
                run_logged([wine, 'start', '/wait', '/unix', patch], 'pc110-update', env, game, 20 * 60)
            finally:
                server = Path(wine).with_name('wineserver')
                if server.is_file():
                    subprocess.run([str(server), '-k'], env=env, capture_output=True, timeout=30)
        if sha256_file(child_named(game, 'halo.exe')) != HALO_SHA256:
            raise SetupError('The installed executable is not the supported PC 1.10 binary. '
                             'Setup will not generate code from an unknown executable.')
    return game, prefix


def selected_game_files(source):
    selected = []
    for name in ('halo.exe', 'strings.dll'):
        selected.append((child_named(source, name), name))
    # Keep runtime configuration and shipped cinematics when present. These
    # are game assets, unlike saved profiles and installation registry data.
    for name in ('config.txt', 'bungie.bik', 'gearbox.bik', 'mgs.bik', 'ending.bik'):
        if not any(p.name.casefold() == name for p in source.iterdir()):
            continue
        file = child_named(source, name)
        if not file.is_file():
            raise SetupError('Optional game content must be a regular file: ' + name)
        selected.append((file, name))
    for folder in ('maps', 'shaders'):
        directory = child_named(source, folder)
        if not directory.is_dir():
            raise SetupError(f'{folder} must be a directory')
        for file in sorted(directory.rglob('*')):
            if file.is_symlink():
                raise SetupError('Game asset trees must not contain symbolic links')
            if file.is_file():
                if folder == 'maps' and file.suffix.lower() != '.map':
                    continue
                if folder == 'shaders' and file.suffix.lower() != '.bin':
                    continue
                selected.append((file, folder + '/' + str(file.relative_to(directory)).lower()))
    names = [name for _, name in selected]
    if len(names) != len(set(names)):
        raise SetupError('Game files collide after Windows filename normalization')
    for file, _ in selected:
        if not file.is_file() or file.is_symlink():
            raise SetupError('Game assets must be regular files')
    return selected


def check_game(root):
    # Validate the registry without ever reporting its values.
    selected = selected_game_files(root)
    expected = {relative for _, relative in selected} | {'halo-vision-registry.txt'}
    actual = {str(p.relative_to(root)).lower() for p in root.rglob('*') if p.is_file()}
    if actual != expected or any(p.is_symlink() for p in root.rglob('*')):
        raise SetupError('game/ contains unexpected files. Import your installation into a fresh checkout with --game-dir.')
    read_registry(root / 'halo-vision-registry.txt')
    result = validate_game(root)
    for name in CAMPAIGN:
        file = root / 'maps' / (name + '.map')
        if not file.is_file() or file.stat().st_size < 1_000_000:
            raise SetupError(f'Campaign map {name}.map is missing or incomplete')
    return result


def import_game(source, registry_source):
    source = source.expanduser().resolve(strict=True)
    seed = read_registry(registry_source)
    if source == GAME.resolve():
        # A manually prepared game directory is never rewritten.
        if not (GAME / 'halo-vision-registry.txt').is_file():
            private_write(GAME / 'halo-vision-registry.txt', seed)
        elif read_registry(GAME / 'halo-vision-registry.txt') != seed:
            raise SetupError('The supplied registry differs from game/; existing private values were preserved.')
        check_game(GAME)
        return
    selected = selected_game_files(source)
    if GAME.exists():
        try:
            _, rows, _, _ = check_game(GAME)
            existing = {row['path']: (row['bytes'], row['sha256']) for row in rows
                        if row['path'] != 'halo-vision-registry.txt'}
            incoming = {relative: (file.stat().st_size, sha256_file(file)) for file, relative in selected}
            if existing == incoming and read_registry(GAME / 'halo-vision-registry.txt') == seed:
                print('Existing game/ matches these inputs; resuming without rewriting it.', flush=True)
                return
        except (SetupError, PreparationError, RegistryError, OSError):
            pass
        raise SetupError('game/ already exists and differs; it was left unchanged. Use a fresh checkout to import another installation.')
    with tempfile.TemporaryDirectory(prefix='import-', dir=LOCAL) as temp:
        stage = Path(temp) / 'game'
        stage.mkdir(mode=0o700)
        for file, relative in selected:
            dest = stage / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            clone_copy(file, dest)
        private_write(stage / 'halo-vision-registry.txt', seed)
        check_game(stage)
        stage.rename(GAME)
    print('Owned game files and private installation values imported and verified.', flush=True)


def stage_payload():
    _, rows, total, digest = check_game(GAME)
    target = LOCAL / 'GamePayload'
    manifest_path = LOCAL / 'GamePayloadManifest.json'
    if target.exists() or manifest_path.exists():
        # Resume only an exact valid payload; do not erase modified game assets.
        try:
            current = validate_bundled_game_payload(LOCAL)
            if current['payload_id'] == digest:
                print('Verified existing bundled game payload; reusing it.', flush=True)
                return
        except Exception:
            pass
        raise SetupError('Existing .setup/GamePayload differs or is incomplete; preserve it and use a fresh checkout.')
    with tempfile.TemporaryDirectory(prefix='payload-', dir=LOCAL) as temp:
        staged = Path(temp) / 'GamePayload'
        staged.mkdir(mode=0o700)
        for row in rows:
            relative = row['path']
            destination = staged / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            clone_copy(GAME / row.get('source_path', relative), destination)
        manifest = {'formatVersion': 1, 'payloadID': digest, 'fileCount': len(rows),
                    'totalBytes': total, 'executableSHA256': HALO_SHA256, 'files': manifest_rows(rows)}
        private_write(Path(temp) / 'GamePayloadManifest.json', json.dumps(manifest, indent=2) + '\n')
        validate_bundled_game_payload(Path(temp))
        staged.rename(target)
        (Path(temp) / 'GamePayloadManifest.json').rename(manifest_path)
    print('Game payload is ready for Xcode to include in your private app.', flush=True)


def python312():
    if sys.version_info[:2] == (3, 12):
        return sys.executable
    return shutil.which('python3.12')


def build_engine(stage, no_open):
    from visual_assets import ensure_visual_assets
    ensure_visual_assets()
    python = python312()
    if not python:
        raise SetupError('Install Python 3.12 first: brew install python@3.12')
    venv = ROOT / '.venv'
    if not venv.exists():
        run_logged([python, '-m', 'venv', venv], 'python-environment')
    python = venv / 'bin/python'
    if not python.is_file():
        raise SetupError('.venv is incomplete; preserve it and recreate it with Python 3.12')
    version = subprocess.check_output([str(python), '-c', 'import sys; print("%d.%d" % sys.version_info[:2])'], text=True).strip()
    if version != '3.12':
        raise SetupError('This checkout has a different Python version in .venv; use a Python 3.12 environment.')
    requirement_hash = sha256_file(ROOT / 'requirements-development.txt')
    stamp = LOCAL / 'requirements.sha256'
    imports_ok = subprocess.run([str(python), '-c', 'import capstone, pefile, numpy, PIL, unicorn'],
                                capture_output=True).returncode == 0
    if not imports_ok or not stamp.exists() or stamp.read_text().strip() != requirement_hash:
        run_logged([python, '-m', 'pip', 'install', '--no-cache-dir', '-r', ROOT / 'requirements-development.txt'], 'python-dependencies')
        private_write(stamp, requirement_hash + '\n')
    env = dict(os.environ, HALO_EXE=str(GAME / 'halo.exe'))
    fingerprint = hashlib.sha256((HALO_SHA256 + requirement_hash).encode())
    inputs = sorted((ROOT / 'tools').rglob('*.py')) + sorted(p for p in (ROOT / 'third_party/xwa').rglob('*')
                                                           if p.is_file() and p.suffix in {'.py', '.h'})
    inputs += sorted((ROOT / 'decompilation').rglob('*.txt'))
    for file in inputs:
        fingerprint.update(str(file.relative_to(ROOT)).encode())
        fingerprint.update(file.read_bytes())
    stamp = LOCAL / 'generation.sha256'
    complete = len(list(GEN.glob('chunk_*.c'))) == 32 and all((GEN / name).is_file() for name in
               ('engine_bundle.c', 'engine_imports.c', 'engine_functions.h', 'generation.json'))
    if not complete or not stamp.exists() or stamp.read_text().strip() != fingerprint.hexdigest():
        run_logged([python, ROOT / 'tools/generate_engine_reuse.py',
                    '@decompilation/c9acf0c46954/function-addresses.txt',
                    '@decompilation/c9acf0c46954/extra-function-entries.txt',
                    '--label', 'whole-exe', '--max-functions', '10000', '--trap-unsupported',
                    '--discover', '--chunks', '32'], 'engine-generation', env)
        run_logged([python, ROOT / 'tools/export_engine_imports.py'], 'engine-imports', env)
        private_write(stamp, fingerprint.hexdigest() + '\n')
    stage_payload()
    if stage == 'build':
        run_logged([python, ROOT / 'tools/build_engine_vision.py', '--configuration', 'Release', '--direct'], 'unsigned-build', env)
        app = ROOT / 'native/EngineVision/.build/DirectXROS/HaloVision.app'
        # Explicit setup invocation adds owned data; the ordinary build command stays source-only.
        if (app / 'GamePayload').exists():
            current = validate_bundled_game_payload(app)
            wanted = validate_bundled_game_payload(LOCAL)
            if current['payload_id'] != wanted['payload_id']:
                raise SetupError('Existing app payload differs; use a clean unsigned build before repeating setup.')
        else:
            shutil.copytree(LOCAL / 'GamePayload', app / 'GamePayload', copy_function=clone_copy)
            clone_copy(LOCAL / 'GamePayloadManifest.json', app / 'GamePayloadManifest.json')
        validate_bundled_game_payload(app)
        print('Unsigned app with your owned data: native/EngineVision/.build/DirectXROS/HaloVision.app\n'
              'This is not yet signed or installed. See docs/SETUP.md for the Xcode signing step.')
    else:
        if not PROJECT.exists():
            run_logged([python, ROOT / 'tools/build_engine_vision.py', '--generate-only'], 'xcode-project', env)
        else:
            text = (PROJECT / 'project.pbxproj').read_text()
            if 'GamePayloadManifest.json' not in text or 'TextureMods.hvt' not in text or 'ShaderMods.hvs' not in text:
                raise SetupError('Existing Xcode project predates setup resources. Preserve its signing choices, '
                                 'then regenerate it with tools/build_engine_vision.py --generate-only.')
            print('Keeping the existing Xcode project and its signing choices.', flush=True)
        print('Ready in Xcode:\n  1. Select EngineVision → Signing & Capabilities.\n'
              '  2. Select your Team and a unique Bundle Identifier; enable automatic signing.\n'
              '  3. Select your paired, unlocked Vision Pro and press Run.\n'
              'Game files are already included. Sign-in, trust and device unlock stay with you.', flush=True)
        if not no_open:
            subprocess.run(['open', str(PROJECT)], check=True)


def quest_commands(args):
    """Ordered Quest packaging command list for issue #1 phase 6.
    Pure: --dry-run prints these, a real run executes them with run_logged.
    Native .so libs come from the arm64 NDK build (phase 3/4) staged into
    the gitignored jniLibs dir; Gradle assembles the APK; adb installs it
    and pushes the gitignored game/ payload to app-specific storage.
    Each entry is (label, argv, cwd)."""
    gradle = args.gradle or shutil.which('gradle') or str(QUEST_ANDROID / 'gradlew')
    adb = args.adb or shutil.which('adb') or 'adb'
    android_dir = ROOT / 'native/EngineHost/android'
    cmds = [
        ('quest-native-libs', ['make', '-C', str(android_dir),
         '-j', str(os.cpu_count() or 4), 'jniLibs'], ROOT),
        ('quest-apk', [gradle, '--no-daemon', '-q', 'assembleDebug'], QUEST_ANDROID),
    ]
    if not args.no_install:
        cmds.append(('quest-adb-install', [adb, 'install', '-r', str(QUEST_APK)], QUEST_ANDROID))
        cmds.append(('quest-push-game', [adb, 'push', str(GAME) + '/.',
         QUEST_DEVICE_GAME], ROOT))
    return cmds


def quest_stage(args):
    """Build the Quest 3 APK and (unless --dry-run) sideload the payload.
    Requires an Android SDK (Gradle) and, for install/push, one adb device;
    none of that runs under --dry-run, which only prints the plan."""
    for label, command, cwd in quest_commands(args):
        if args.dry_run:
            print(f'[dry-run] {label}: cd {cwd} && ' + ' '.join(map(str, command)), flush=True)
            continue
        if not (ROOT / 'native/build/android-arm64/apk/jniLibs/arm64-v8a/libhaloquest.so').is_file() \
                and label == 'quest-apk':
            raise SetupError('libhaloquest.so is not staged in jniLibs; the native build stage failed.')
        run_logged(command, label, cwd=cwd)
    if args.dry_run:
        print('Quest dry-run: nothing was built, installed, or pushed.', flush=True)
        return
    if args.no_install:
        print(f'Quest APK built: {QUEST_APK.relative_to(ROOT)}\n'
              'This proves packaging only; no install or push was run (--no-install).', flush=True)
    else:
        print(f'Quest APK built: {QUEST_APK.relative_to(ROOT)}\n'
              f'Installed {QUEST_PACKAGE} and pushed the game payload to {QUEST_DEVICE_GAME}.\n'
              'This proves install + payload staging, not on-device rendering; '
              'run the checklist in docs/VALIDATION.md.', flush=True)
def doctor(args):
    checks = []
    def add(name, ok, action=''):
        checks.append({'check': name, 'ok': bool(ok), 'action': '' if ok else action})
    add('Apple Silicon macOS', sys.platform == 'darwin' and platform.machine() == 'arm64', 'Use an Apple Silicon Mac for visionOS builds.')
    add('Python 3.12', python312(), 'Install Python 3.12: brew install python@3.12')
    sdk = subprocess.run(['xcrun', '--sdk', 'xros', '--show-sdk-version'], capture_output=True, text=True) if shutil.which('xcrun') else None
    add('visionOS SDK', sdk is not None and sdk.returncode == 0, 'Install Xcode and its visionOS platform; select Xcode in Settings → Locations.')
    add('XcodeGen', shutil.which('xcodegen'), 'Install XcodeGen: brew install xcodegen (not needed for --stage build).')
    add('12 GiB free disk', shutil.disk_usage(ROOT).free >= 12 * GIB, 'Free disk space before running the installer or building.')
    if not args.game_dir and not GAME.exists() and not args.wine_prefix:
        add('Wine for original installer', find_wine(args.wine), 'Install a compatible macOS Wine distribution, or use the Windows fallback in docs/SETUP.md.')
    source = args.game_dir or (prefix_game(args.wine_prefix) if args.wine_prefix else None) or (GAME if GAME.exists() else None)
    if source:
        try:
            files = dict((relative, file) for file, relative in selected_game_files(source))
            for name, minimum in REQUIRED_GAME_FILES.items():
                if name == 'halo-vision-registry.txt':
                    continue
                if name not in files or files[name].stat().st_size < minimum:
                    raise SetupError(f'Game installation is missing or has incomplete {name}')
            for name in CAMPAIGN:
                if f'maps/{name}.map' not in files:
                    raise SetupError(f'Game installation is missing {name}.map')
            if sha256_file(files['halo.exe']) != HALO_SHA256:
                raise SetupError('Executable does not match the supported retail PC 1.10 SHA-256')
            add('Existing game installation', True)
            read_registry(args.registry or args.wine_prefix or source / 'halo-vision-registry.txt')
            add('Private Halo registry', True)
        except (OSError, SetupError, RegistryError) as error:
            add('Existing game and private registry', False, str(error))
    elif args.wine_prefix:
        add('Existing Wine installation', False, 'No default Halo path found; supply --game-dir.')
    if args.iso:
        if sys.platform != 'darwin':
            add('Retail ISO', False, 'ISO inspection requires macOS hdiutil.')
        else:
            try:
                with mounted_iso(args.iso) as mount:
                    disc = inspect_disc(mount)
                add('Retail ISO', True)
                add('PC 1.10 update input', not disc['needsPC110Update'] or (args.patch and args.patch.is_file()),
                    'Supply the retail PC 1.10 update installer with --patch when prompted; the resulting executable is hash-checked.')
            except (SetupError, OSError, subprocess.SubprocessError) as error:
                add('Retail ISO', False, str(error))
    return {'ready': all(c['ok'] for c in checks), 'checks': checks,
            'next': './setup.sh <your ISO> (or --game-dir <your installed copy>)',
            'note': 'A valid product key must be entered in the original installer. Apple signing is completed in Xcode.'}


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', nargs='?', type=Path, help='Your original Halo PC retail ISO; omit to choose it in a file picker')
    parser.add_argument('--stage', choices=('check', 'prepare', 'xcode', 'build', 'Quest'), default='xcode', help='Default: prepare owned game, generate sources and open Xcode; Quest builds the Android APK (issue #1)')
    parser.add_argument('--game-dir', type=Path, help='Import an existing, owned PC 1.10 installation instead of running the ISO installer')
    parser.add_argument('--bundled', action='store_true', help='Use the Complete release game data; supply your own --registry or --wine-prefix')
    parser.add_argument('--registry', type=Path, help='Your Halo registry seed or Windows .reg export (never a raw product key)')
    parser.add_argument('--wine-prefix', type=Path, help='Read game/registry from an existing Wine prefix; never modified')
    parser.add_argument('--wine', help='Path to a macOS Wine binary for ISO installation')
    parser.add_argument('--patch', type=Path, help='Your retail halopc-patch-1.0.10.exe updater')
    parser.add_argument('--non-interactive', action='store_true', help='Never start an installer UI or ask questions; report missing human steps')
    parser.add_argument('--no-open', action='store_true', help='Prepare Xcode without opening it')
    parser.add_argument('--json', action='store_true', help='Machine-readable --stage check output')
    parser.add_argument('--dry-run', action='store_true', help='--stage Quest: print the build/install plan, run nothing')
    parser.add_argument('--gradle', help='--stage Quest: gradle binary (default: PATH, then android/gradlew)')
    parser.add_argument('--adb', help='--stage Quest: adb binary (default: PATH)')
    parser.add_argument('--no-install', action='store_true', help='--stage Quest: build the APK, skip adb install/push')
    args = parser.parse_args(argv)
    if args.bundled:
        if args.iso or args.game_dir:
            parser.error('--bundled cannot be combined with an ISO or --game-dir')
        if not args.registry and not args.wine_prefix:
            parser.error('--bundled requires your private --registry or --wine-prefix')
        args.game_dir = ROOT / 'Release/HaloVision.app/GamePayload'
    if args.json and args.stage != 'check':
        parser.error('--json is available with --stage check')
    if args.iso and args.game_dir:
        parser.error('Choose either an ISO or --game-dir')
    for name in ('iso', 'game_dir', 'registry', 'wine_prefix', 'patch'):
        value = getattr(args, name)
        if value:
            setattr(args, name, value.expanduser().resolve())
    return args


def main(argv=None):
    args = parse_args(argv)
    if args.stage == 'check':
        report = doctor(args)
        if args.json:
            print(json.dumps(report, indent=2))
        else:
            for row in report['checks']:
                print(('OK' if row['ok'] else 'NEEDS ACTION') + ': ' + row['check'])
                if row['action']:
                    print('  ' + row['action'])
        return 0 if report['ready'] else 2
    if args.stage == 'Quest':
        if sys.platform != 'darwin' or platform.machine() != 'arm64':
            raise SetupError('The Quest APK build needs the arm64 Android NDK on this Mac.')
        return quest_stage(args)
    if sys.platform != 'darwin' or platform.machine() != 'arm64':
        raise SetupError('Run setup on an Apple Silicon Mac. See docs/SETUP.md for preparing your game on Windows.')
    if args.stage != 'prepare':
        if not python312():
            raise SetupError('Install Python 3.12: brew install python@3.12')
        if args.stage == 'xcode' and not shutil.which('xcodegen'):
            raise SetupError('Install XcodeGen: brew install xcodegen')
        if not shutil.which('xcrun') or subprocess.run(['xcrun', '--sdk', 'xros', '--show-sdk-path'], capture_output=True).returncode:
            raise SetupError('Install Xcode with its visionOS platform before building; run --stage check for details.')
    need_space()
    if LOCAL.is_symlink() or GAME.is_symlink():
        raise SetupError('.setup/ and game/ must not be symbolic links')
    LOCAL.mkdir(exist_ok=True, mode=0o700)
    LOCAL.chmod(0o700)
    with (LOCAL / 'setup.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise SetupError('Another setup process is using this checkout; wait for it to finish.') from None
        source = args.game_dir
        registry = args.registry or args.wine_prefix
        if not source and args.wine_prefix:
            source = prefix_game(args.wine_prefix)
            if not source:
                raise SetupError('No default Halo installation in that prefix. Add --game-dir for its custom path.')
        if source:
            registry = registry or source / 'halo-vision-registry.txt'
            import_game(source, registry)
        elif GAME.is_dir():
            check_game(GAME)
            print('Reusing verified game/. The ISO and any previous installation are unchanged.', flush=True)
        else:
            source, prefix = iso_install(args)
            import_game(source, args.registry or prefix)
        private_write(LOCAL / 'status.json', json.dumps({'stage': 'game-ready', 'executableSHA256': HALO_SHA256}) + '\n')
        if args.stage != 'prepare':
            build_engine(args.stage, args.no_open or args.non_interactive)
            private_write(LOCAL / 'status.json', json.dumps({'stage': args.stage + '-ready', 'signed': False, 'installed': False}) + '\n')
        else:
            print('Game preparation complete. Rerun ./setup.sh to generate the engine and open Xcode.')
    return 0


if __name__ == '__main__':
    os.umask(0o077)
    try:
        raise SystemExit(main())
    except (SetupError, PreparationError, RegistryError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'Setup stopped: {error}', file=sys.stderr)
        print('Your ISO and existing installation were preserved. See docs/SETUP.md.', file=sys.stderr)
        raise SystemExit(2)
    except KeyboardInterrupt:
        print('\nSetup cancelled. Rerun the same command to resume.', file=sys.stderr)
        raise SystemExit(130)
