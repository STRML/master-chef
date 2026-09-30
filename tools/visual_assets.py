#!/usr/bin/env python3
"""Fetch and verify the exact release texture/shader packs before building."""
from __future__ import annotations
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile
import urllib.request

from prepare_engine_vision_device import clone_copy
import zipfile

ROOT = Path(__file__).resolve().parents[1]
NAMES = {'TextureMods.hvt', 'ShaderMods.hvs', 'CEnshineSources.zip'}

def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def verify_files(directory, manifest):
    for row in manifest['files']:
        path = directory / row['path']
        if path.is_symlink() or not path.is_file() or path.stat().st_size != row['bytes'] or digest(path) != row['sha256']:
            raise ValueError('Visual asset is missing or changed: ' + row['path'])

def extract_verified(archive, destination, manifest):
    rows = {row['path']: row for row in manifest['files']}
    if set(rows) != NAMES or len(rows) != len(manifest['files']):
        raise ValueError('Unexpected visual asset manifest entries')
    with zipfile.ZipFile(archive) as zipped:
        if len(zipped.namelist()) != len(rows) or set(zipped.namelist()) != set(rows):
            raise ValueError('Unexpected or duplicate archive entries')
        for info in zipped.infolist():
            row = rows[info.filename]
            if stat.S_ISLNK(info.external_attr >> 16) or info.file_size != row['bytes']:
                raise ValueError('Invalid archive entry: ' + info.filename)
            target = destination / info.filename
            with zipped.open(info) as source, target.open('xb') as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
            target.chmod(0o600)
    verify_files(destination, manifest)

def ensure_visual_assets(root=ROOT, archive=None):
    root = Path(root)
    manifest = json.loads((root / 'mods/visual-assets.json').read_text())
    if {row['path'] for row in manifest['files']} != NAMES:
        raise ValueError('Unexpected visual asset manifest')
    local = root / '.setup'
    target = local / 'VisualMods'
    if local.is_symlink() or target.is_symlink():
        raise ValueError('Visual asset directories must not be symbolic links')
    local.mkdir(exist_ok=True, mode=0o700)
    with (local / 'visual-assets.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if target.exists():
            verify_files(target, manifest)
            return target
        bundled = root / 'Release/HaloVision.app'
        if bundled.is_dir():
            verify_files(bundled, manifest)
            with tempfile.TemporaryDirectory(prefix='visual-assets-', dir=local) as temporary:
                staging = Path(temporary) / 'unpacked'
                staging.mkdir()
                for row in manifest['files']:
                    clone_copy(bundled / row['path'], staging / row['path'])
                (staging / 'VisualModsManifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
                staging.rename(target)
            return target
        needed = sum(row['bytes'] for row in manifest['files']) + manifest['archive']['bytes'] + 1024**3
        if shutil.disk_usage(local).free < needed:
            raise ValueError('Insufficient free space for the verified visual assets and 1 GiB reserve')
        with tempfile.TemporaryDirectory(prefix='visual-assets-', dir=local) as temporary:
            temporary = Path(temporary)
            source = Path(archive) if archive else temporary / 'download.zip'
            if archive is None:
                url = manifest['archive']['url']
                if not url.startswith('https://github.com/mitchaiet/master-chef/releases/download/'):
                    raise ValueError('Unexpected visual asset download origin')
                print('Downloading the matching visual pack (about %.1f GiB)...' % (manifest['archive']['bytes'] / 1024**3), flush=True)
                request = urllib.request.Request(url, headers={'User-Agent': 'MasterChef-Setup'})
                with urllib.request.urlopen(request, timeout=120) as response, source.open('xb') as output:
                    count = 0
                    while block := response.read(1024 * 1024):
                        count += len(block)
                        if count > manifest['archive']['bytes']:
                            raise ValueError('Visual archive exceeds its expected size')
                        output.write(block)
            if source.stat().st_size != manifest['archive']['bytes'] or digest(source) != manifest['archive']['sha256']:
                raise ValueError('Visual archive checksum mismatch')
            staging = temporary / 'unpacked'
            staging.mkdir()
            extract_verified(source, staging, manifest)
            (staging / 'VisualModsManifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            staging.rename(target)
        print('Verified %s textures and %s shader replacements.' % (manifest['textureEntries'], manifest['shaderEntries']), flush=True)
    return target

def bundle_visual_assets(product, directory):
    manifest = json.loads((directory / 'VisualModsManifest.json').read_text())
    verify_files(directory, manifest)
    for name in sorted(NAMES | {'VisualModsManifest.json'}):
        source, destination = directory / name, Path(product) / name
        if destination.exists():
            if destination.is_symlink() or digest(destination) != digest(source):
                raise ValueError('Existing app has different visual assets; rebuild with --clean')
        else:
            clone_copy(source, destination)
    verify_files(Path(product), manifest)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, help='Use the matching downloaded ZIP instead of fetching it')
    args = parser.parse_args()
    try:
        ensure_visual_assets(archive=args.archive)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(2, 'Visual asset setup failed: %s\n' % error)

if __name__ == '__main__':
    main()
