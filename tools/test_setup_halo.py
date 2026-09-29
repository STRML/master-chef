#!/usr/bin/env python3
"""Source-only setup regressions. No ISO, product key, Wine, signing or device required."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import halo_setup_registry as reg
import setup_halo as setup
import prepare_engine_vision_device as prep


def seed():
    # Deliberately synthetic data, never usable as a product key.
    binary = bytes(range(1, 33)).hex()
    return f'HKLM|{reg.KEY}|DigitalProductID|3|{binary}\nHKLM|{reg.KEY}|PID|1|TEST-ONLY\n'


class RegistryTests(unittest.TestCase):
    def test_seed_clean_exit_and_no_foreign_keys(self):
        text = seed() + 'HKLM|Software\\Other|Password|1|DO-NOT-EXPORT\n'
        result = reg.finish(reg.parse_export(text))
        self.assertIn('ExitFlag|1|clean', result)
        self.assertNotIn('DO-NOT-EXPORT', result)

    def test_windows_wrapped_utf16_and_wow64(self):
        binary = ','.join(f'{n:02x}' for n in range(1, 33))
        text = ('Windows Registry Editor Version 5.00\r\n'
                '[HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\Microsoft Games\\Halo]\r\n'
                '"PID"="TEST-ONLY"\r\n"DigitalProductID"=hex:' + binary[:48] + '\\\r\n  ' + binary[48:] + '\r\n')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'owned.reg'
            path.write_bytes(text.encode('utf-16'))
            result = reg.read_registry(path)
        self.assertIn('DigitalProductID|3|' + bytes(range(1, 33)).hex(), result)

    def test_wine_registry_and_unrelated_sections(self):
        text = ('WINE REGISTRY Version 2\n[Software\\\\Microsoft\\\\Microsoft Games\\\\Halo] 0\n'
                '"PID"="TEST-ONLY"\n"DigitalProductID"=hex:' + ','.join(f'{n:02x}' for n in range(1, 33)) +
                '\n"EXE Path"="C:\\\\Program Files\\\\Halo"\n'
                '[Software\\\\Other]\n"PID"="DO-NOT-EXPORT"\n')
        result = reg.finish(reg.parse_export(text, 'HKLM'))
        self.assertIn('EXE Path|1|C:\\Program Files\\Halo', result)
        self.assertNotIn('DO-NOT-EXPORT', result)

    def test_invalid_license_values_never_echoed(self):
        for value in ['<placeholder>', 'z' * 40, '0' * 40, '1' * 33, 'PRIVATE-INPUT']:
            with self.subTest(value=value):
                with self.assertRaises(reg.RegistryError) as error:
                    reg.finish(reg.parse_export(seed().replace(bytes(range(1, 33)).hex(), value)))
                self.assertNotIn(value, str(error.exception))

    def test_missing_pid_rejected(self):
        with self.assertRaises(reg.RegistryError):
            reg.finish(reg.parse_export(seed().splitlines()[0]))

    def test_malformed_private_string_is_not_echoed(self):
        with self.assertRaises(reg.RegistryError) as error:
            reg.decode_string('"PRIVATE-INPUT\\q"')
        self.assertNotIn('PRIVATE-INPUT', str(error.exception))

    def test_private_write_permissions(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'private' / 'registry.txt'
            setup.private_write(path, seed())
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)


class GameImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='halo-setup-tests-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.local = self.root / '.setup'
        self.local.mkdir()
        self.game = self.root / 'game'
        self.source = self.root / 'Owned installation with spaces'
        self.source.mkdir()
        (self.source / 'HALO.EXE').write_bytes(b'synthetic executable fixture')
        (self.source / 'Strings.dll').write_bytes(b'synthetic strings')
        (self.source / 'Maps').mkdir()
        (self.source / 'Shaders').mkdir()
        (self.source / 'Maps/a10.map').write_bytes(b'0' * 1_000_000)
        (self.source / 'Shaders/vsh.bin').write_bytes(b'shader fixture')
        (self.source / 'private-note.txt').write_text('DO-NOT-BUNDLE')
        self.registry = self.root / 'private.reg'
        self.registry.write_text(seed())
        digest = hashlib.sha256((self.source / 'HALO.EXE').read_bytes()).hexdigest()
        for module, name, value in [(setup, 'ROOT', self.root), (setup, 'LOCAL', self.local),
                                    (setup, 'GAME', self.game), (setup, 'CAMPAIGN', ('a10',)),
                                    (setup, 'HALO_SHA256', digest), (prep, 'HALO_SHA256', digest),
                                    (prep, 'REQUIRED_GAME_FILES', {'halo.exe': 1, 'strings.dll': 1,
                                      'maps/a10.map': 1, 'shaders/vsh.bin': 1, 'halo-vision-registry.txt': 1})]:
            p = patch.object(module, name, value)
            p.start(); self.addCleanup(p.stop)

    def test_import_normalizes_and_only_copies_assets(self):
        setup.import_game(self.source, self.registry)
        self.assertTrue((self.game / 'strings.dll').is_file())
        self.assertTrue((self.game / 'maps/a10.map').is_file())
        self.assertFalse((self.game / 'private-note.txt').exists())
        self.assertTrue((self.source / 'HALO.EXE').exists())
        self.assertEqual((self.game / 'halo-vision-registry.txt').stat().st_mode & 0o777, 0o600)

    def test_existing_game_not_overwritten(self):
        self.game.mkdir(); sentinel = self.game / 'save.txt'; sentinel.write_text('preserve')
        with self.assertRaises(setup.SetupError):
            setup.import_game(self.source, self.registry)
        self.assertEqual(sentinel.read_text(), 'preserve')

    def test_matching_import_can_resume(self):
        setup.import_game(self.source, self.registry)
        before = (self.game / 'halo.exe').stat().st_mtime_ns
        setup.import_game(self.source, self.registry)
        self.assertEqual((self.game / 'halo.exe').stat().st_mtime_ns, before)

    def test_different_registry_never_replaces_existing(self):
        setup.import_game(self.source, self.registry)
        original = (self.game / 'halo-vision-registry.txt').read_bytes()
        self.registry.write_text(seed().replace('TEST-ONLY', 'SECOND-TEST'))
        with self.assertRaises(setup.SetupError):
            setup.import_game(self.game, self.registry)
        self.assertEqual((self.game / 'halo-vision-registry.txt').read_bytes(), original)

    def test_failed_import_never_promoted(self):
        with patch.object(setup, 'check_game', side_effect=setup.SetupError('fixture validation failure')):
            with self.assertRaises(setup.SetupError):
                setup.import_game(self.source, self.registry)
        self.assertFalse(self.game.exists())
        self.assertEqual(list(self.local.glob('import-*')), [])

    def test_symlink_escape_rejected(self):
        (self.source / 'Maps/leak.map').symlink_to(self.registry)
        with self.assertRaises(setup.SetupError):
            setup.import_game(self.source, self.registry)
        self.assertFalse(self.game.exists())

    def test_wrong_executable_rejected(self):
        (self.source / 'HALO.EXE').write_bytes(b'unsupported exe')
        with self.assertRaises(prep.PreparationError):
            setup.import_game(self.source, self.registry)
        self.assertFalse(self.game.exists())

    def test_payload_manifest_and_resume(self):
        setup.import_game(self.source, self.registry)
        setup.stage_payload()
        report = prep.validate_bundled_game_payload(self.local)
        self.assertEqual(report['file_count'], 5)
        before = (self.local / 'GamePayloadManifest.json').read_bytes()
        setup.stage_payload()
        self.assertEqual(before, (self.local / 'GamePayloadManifest.json').read_bytes())

    def test_modified_payload_is_not_overwritten(self):
        setup.import_game(self.source, self.registry)
        setup.stage_payload()
        path = self.local / 'GamePayload/maps/a10.map'
        path.write_bytes(b'preserve my changes')
        with self.assertRaises(setup.SetupError):
            setup.stage_payload()
        self.assertEqual(path.read_bytes(), b'preserve my changes')

    def test_unexpected_files_in_game_not_bundled(self):
        setup.import_game(self.source, self.registry)
        (self.game / 'notes.txt').write_text('private')
        with self.assertRaises(setup.SetupError):
            setup.stage_payload()
        self.assertFalse((self.local / 'GamePayload').exists())

    def test_disk_guard(self):
        with patch.object(setup.shutil, 'disk_usage') as usage:
            usage.return_value.free = 2 * setup.GIB
            with self.assertRaises(setup.SetupError):
                setup.need_space()
        self.assertFalse(self.game.exists())


class WorkflowTests(unittest.TestCase):
    def test_noninteractive_never_starts_installer(self):
        args = setup.parse_args(['--non-interactive', '--stage', 'prepare'])
        with patch.object(setup, 'find_wine', return_value='/fixture/wine'), patch.object(setup.subprocess, 'Popen') as launch:
            with self.assertRaises(setup.SetupError):
                setup.iso_install(args)
            launch.assert_not_called()

    def test_iso_detaches_after_bad_disc(self):
        calls = []
        def run(command, **kwargs):
            calls.append(command)
            data = b''
            if command[1] == 'attach':
                mount = command[command.index('-mountpoint') + 1]
                data = plistlib.dumps({'system-entities': [{'mount-point': mount}]})
            return subprocess.CompletedProcess(command, 0, data, b'')
        with tempfile.TemporaryDirectory() as tmp:
            iso = Path(tmp) / 'My Halo.iso'; iso.write_bytes(b'fixture')
            with patch.object(setup.subprocess, 'run', side_effect=run):
                with self.assertRaises(setup.SetupError):
                    with setup.mounted_iso(iso) as mount:
                        setup.inspect_disc(mount)
        self.assertIn('-readonly', calls[0])
        self.assertEqual(calls[-1][1], 'detach')
        self.assertEqual(calls[-1][2], calls[0][calls[0].index('-mountpoint') + 1])

    def test_iso_detaches_on_cancellation(self):
        def run(command, **kwargs):
            data = plistlib.dumps({'system-entities': [{'mount-point': command[command.index('-mountpoint') + 1]}]}) if command[1] == 'attach' else b''
            return subprocess.CompletedProcess(command, 0, data, b'')
        with tempfile.TemporaryDirectory() as tmp:
            iso = Path(tmp) / 'Halo.iso'; iso.write_bytes(b'fixture')
            with patch.object(setup.subprocess, 'run', side_effect=run) as invoke:
                with self.assertRaises(KeyboardInterrupt):
                    with setup.mounted_iso(iso):
                        raise KeyboardInterrupt()
                self.assertEqual(invoke.call_args.args[0][1], 'detach')

    def test_failed_child_is_not_success(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(setup, 'LOCAL', Path(tmp)):
            with self.assertRaises(setup.SetupError):
                setup.run_logged([__import__('sys').executable, '-c', 'raise SystemExit(7)'], 'fixture-failure')
            self.assertEqual((Path(tmp) / 'logs/fixture-failure.log').stat().st_mode & 0o777, 0o600)

    def test_public_project_includes_optional_payload_and_signing(self):
        text = (setup.ROOT / 'native/EngineVision/project.yml').read_text()
        self.assertIn('CODE_SIGNING_ALLOWED: YES', text)
        self.assertIn('../../.setup/GamePayloadManifest.json', text)
        self.assertIn('optional: true', text)

    def test_conflicting_sources_are_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            setup.parse_args(['Halo.iso', '--game-dir', 'already-installed'])


if __name__ == '__main__':
    unittest.main()
