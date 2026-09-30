#!/usr/bin/env python3
"""Exercise archive integrity, extraction boundaries and stale-pack preservation."""
from pathlib import Path
import hashlib
import json
import stat
import tempfile
import unittest
import zipfile
from unittest.mock import patch
import visual_assets as assets
from setup_halo import selected_game_files

class VisualAssetsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'mods').mkdir()
        self.archive = self.root / 'pack.zip'
        self.contents = {name: (name + ':fixture').encode() for name in assets.NAMES}
        self.manifest = {'textureEntries':1, 'shaderEntries':1, 'files':[
            {'path':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
            for name,data in sorted(self.contents.items())]}
        self.write_archive()

    def write_archive(self, extra=None, symlink=False):
        with zipfile.ZipFile(self.archive, 'w') as zipped:
            for name,data in self.contents.items():
                info = zipfile.ZipInfo(name)
                if symlink: info.external_attr = (stat.S_IFLNK | 0o777) << 16
                zipped.writestr(info, data)
            if extra: zipped.writestr(extra, b'unexpected')
        self.manifest['archive'] = {'bytes':self.archive.stat().st_size,
            'sha256':assets.digest(self.archive),'url':'https://github.com/mitchaiet/master-chef/releases/download/test/fixture.zip'}
        (self.root / 'mods/visual-assets.json').write_text(json.dumps(self.manifest))

    def test_exact_pack_resume_and_bundle(self):
        target = assets.ensure_visual_assets(self.root, self.archive)
        with patch.object(assets.urllib.request, 'urlopen', side_effect=AssertionError('network on resume')):
            self.assertEqual(assets.ensure_visual_assets(self.root), target)
        app = self.root / 'app'; app.mkdir()
        assets.bundle_visual_assets(app, target)
        assets.verify_files(app, self.manifest)

    def test_bad_archive_hash_leaves_no_install(self):
        with self.archive.open('ab') as f: f.write(b'tampered')
        with self.assertRaisesRegex(ValueError, 'checksum'): assets.ensure_visual_assets(self.root, self.archive)
        self.assertFalse((self.root / '.setup/VisualMods').exists())

    def test_modified_existing_pack_is_preserved(self):
        target = assets.ensure_visual_assets(self.root, self.archive)
        file = target / 'TextureMods.hvt'; file.write_bytes(b'custom')
        with self.assertRaises(ValueError): assets.ensure_visual_assets(self.root, self.archive)
        self.assertEqual(file.read_bytes(), b'custom')

    def test_traversal_entry_rejected(self):
        self.write_archive('../escape')
        with self.assertRaisesRegex(ValueError, 'entries'): assets.ensure_visual_assets(self.root, self.archive)
        self.assertFalse((self.root / '.setup/escape').exists())

    def test_symlink_entry_rejected(self):
        self.write_archive(symlink=True)
        with self.assertRaisesRegex(ValueError, 'entry'): assets.ensure_visual_assets(self.root, self.archive)

    def test_wrong_inner_content_rejected_after_outer_hash_matches(self):
        self.contents['ShaderMods.hvs'] = b'wrong'
        self.write_archive()
        with self.assertRaises(ValueError): assets.ensure_visual_assets(self.root, self.archive)
        self.assertFalse((self.root / '.setup/VisualMods').exists())

    def test_symlink_install_location_rejected(self):
        other = self.root / 'other'; other.mkdir()
        (self.root / '.setup').symlink_to(other, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symbolic'): assets.ensure_visual_assets(self.root, self.archive)

    def test_complete_bundle_works_without_download(self):
        bundled = self.root / 'Release/HaloVision.app'; bundled.mkdir(parents=True)
        for name,data in self.contents.items(): (bundled / name).write_bytes(data)
        with patch.object(assets.urllib.request, 'urlopen', side_effect=AssertionError('network despite bundled assets')):
            target = assets.ensure_visual_assets(self.root)
        assets.verify_files(target, self.manifest)

    def test_game_content_selection_preserves_configuration_and_movies_only(self):
        for name in ('halo.exe', 'strings.dll', 'config.txt', 'ending.bik', 'bungie.bik', 'gearbox.bik', 'mgs.bik', 'personal.sav', 'halo-vision-registry.txt'):
            (self.root / name).write_bytes(b'fixture')
        for folder in ('maps','shaders'):
            (self.root / folder).mkdir()
        (self.root / 'maps/a10.map').write_bytes(b'fixture')
        (self.root / 'shaders/fx.bin').write_bytes(b'fixture')
        names = {name for _,name in selected_game_files(self.root)}
        self.assertEqual(names, {'halo.exe','strings.dll','config.txt','ending.bik','bungie.bik','gearbox.bik','mgs.bik','maps/a10.map','shaders/fx.bin'})

if __name__ == '__main__':
    unittest.main()
