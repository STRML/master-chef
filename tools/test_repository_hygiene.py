#!/usr/bin/env python3
"""Ensure the documentation artwork exception cannot admit arbitrary binaries."""
from pathlib import Path
import tempfile
import unittest
from check_repository_hygiene import ROOT, audit


class ArtworkHygieneTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.path = self.root / 'docs/assets/master-chef-header.png'
        self.path.parent.mkdir(parents=True)
        self.art = (ROOT / 'docs/assets/master-chef-header.png').read_bytes()

    def test_exact_reviewed_art(self):
        self.path.write_bytes(self.art)
        self.assertEqual(audit(self.root, strict=True)[1], [])

    def test_changed_art_rejected(self):
        self.path.write_bytes(self.art + b'changed')
        self.assertEqual(audit(self.root, strict=True)[1][0][2], 'unreviewed-documentation-art')

    def test_different_path_rejected(self):
        self.path.with_name('unreviewed.png').write_bytes(self.art)
        self.assertEqual(audit(self.root, strict=True)[1][0][2], 'binary-file')

    def test_text_cannot_hide_under_image_name(self):
        self.path.write_text('unreviewed private input')
        self.assertEqual(audit(self.root, strict=True)[1][0][2], 'unreviewed-documentation-art')



class QuestHygieneTests(unittest.TestCase):
    """The Quest packaging guards must reject game payload and signing
    material in the build-staging paths the source audit skips."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def kinds(self):
        return [kind for _path, _line, kind in audit(self.root)[1]]

    def put_jnilib(self, abi, name):
        path = self.root / 'native/build/android-arm64/apk/jniLibs' / abi / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'\x7fELF stub')
        return path

    def test_allowed_libs_pass(self):
        for name in ('libhaloquest.so', 'libopenxr.so', 'libc++_shared.so'):
            self.put_jnilib('arm64-v8a', name)
        self.assertEqual(self.kinds(), [])

    def test_game_data_in_jnilibs_rejected(self):
        self.put_jnilib('arm64-v8a', 'bungie.bik')
        self.assertIn('quest-jnilibs-game-data', self.kinds())

    def test_foreign_abi_rejected(self):
        self.put_jnilib('armeabi-v7a', 'libopenxr.so')
        self.assertIn('quest-jnilibs-abi', self.kinds())

    def test_unexpected_lib_rejected(self):
        self.put_jnilib('arm64-v8a', 'libmystery.so')
        self.assertIn('quest-jnilibs-unexpected-lib', self.kinds())

    def test_keystore_rejected(self):
        path = self.root / 'android/app/release.keystore'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'not a real keystore')
        self.assertIn('signing-material', self.kinds())

    def test_gradle_signing_config_rejected(self):
        path = self.root / 'android/app/build.gradle'
        path.parent.mkdir(parents=True)
        path.write_text('signingConfig {\n    storeFile file("release.jks")\n}\n')
        self.assertIn('android-signing-config', self.kinds())

    def write_apk(self, entries):
        import zipfile
        path = self.root / 'android/app/build/outputs/apk/debug/app-debug.apk'
        path.parent.mkdir(parents=True)
        with zipfile.ZipFile(path, 'w') as apk:
            for name in entries:
                apk.writestr(name, b'stub')
        return path

    def test_clean_apk_passes(self):
        self.write_apk(['AndroidManifest.xml', 'classes.dex',
                        'lib/arm64-v8a/libhaloquest.so', 'resources.arsc'])
        self.assertEqual([k for k in self.kinds() if k.startswith('apk-')], [])

    def test_apk_game_entry_rejected(self):
        self.write_apk(['AndroidManifest.xml', 'assets/maps/a10.map'])
        self.assertIn('apk-game-data', self.kinds())

    def test_apk_root_game_binary_rejected(self):
        self.write_apk(['halo.exe'])
        self.assertIn('apk-game-data', self.kinds())

    def test_apk_foreign_abi_rejected(self):
        self.write_apk(['lib/x86_64/libopenxr.so'])
        self.assertIn('apk-abi', self.kinds())

if __name__ == '__main__':
    unittest.main()
