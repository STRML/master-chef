"""Read only Halo installation values; never print product information."""
from __future__ import annotations

import json
from pathlib import Path
import re

KEY = r'Software\Microsoft\Microsoft Games\Halo'
ALLOWED = {'CDPath', 'DigitalProductID', 'DistID', 'EXE Path', 'InstalledGroup',
           'LangID', 'Launched', 'PendingVersion', 'PID', 'Version', 'VersionType',
           'default', 'ExitFlag', 'FIRSTRUN', 'gamma'}


class RegistryError(ValueError):
    pass


def normalize_key(value):
    value = value.replace('\\\\', '\\')
    return re.sub(r'(?i)software\\wow6432node\\', lambda _: 'Software\\', value)


def decode_string(value):
    # Registry escapes are a subset of JSON string escapes. Reject malformed
    # values instead of echoing private input in an exception.
    try:
        return json.loads(value)
    except (ValueError, TypeError):
        raise RegistryError('Malformed string in Halo registry export') from None


def finish(rows):
    lookup = {(root, name): (kind, value) for root, key, name, kind, value in rows}
    binary = lookup.get(('HKLM', 'DigitalProductID'))
    pid = lookup.get(('HKLM', 'PID'))
    if (not binary or binary[0] != '3' or not re.fullmatch(r'[0-9a-fA-F]{32,1024}', binary[1])
            or len(binary[1]) % 2 or not any(c != '0' for c in binary[1])):
        raise RegistryError('Missing or invalid DigitalProductID; use your own completed Halo installation')
    if not pid or pid[0] != '1' or not pid[1].strip() or any(c in pid[1] for c in '<>'):
        raise RegistryError('Missing PID; use your own completed Halo installation')
    result = {}
    for root, key, name, kind, value in rows:
        if root not in {'HKLM', 'HKCU'} or key != KEY or name not in ALLOWED:
            continue
        if kind not in {'1', '3', '4'} or any(c in value for c in '\r\n|\0'):
            raise RegistryError('Unsupported value in Halo registry export')
        if kind == '3' and (len(value) % 2 or not re.fullmatch('[0-9a-fA-F]*', value)):
            raise RegistryError('Malformed binary value in Halo registry export')
        if kind == '4' and (not value.isdecimal() or not 0 <= int(value) <= 0xffffffff):
            raise RegistryError('Malformed DWORD in Halo registry export')
        # Runtime registry buffers are bounded; never silently truncate a key.
        if kind == '1' and len(value.encode()) >= 512:
            raise RegistryError('Oversized value in Halo registry export')
        result[(root, name)] = (root, KEY, name, kind, value)
    result[('HKCU', 'ExitFlag')] = ('HKCU', KEY, 'ExitFlag', '1', 'clean')
    return '\n'.join('|'.join(row) for _, row in sorted(result.items())) + '\n'


def parse_export(text, default_root=None):
    rows = []
    if text.lstrip().startswith(('HKLM|', 'HKCU|')):
        for line in text.splitlines():
            parts = line.split('|')
            if len(parts) != 5:
                raise RegistryError('Malformed Halo registry seed')
            root, key, name, kind, value = parts
            key = normalize_key(key)
            if key.casefold() == KEY.casefold() and name in ALLOWED:
                rows.append((root, KEY, name, kind, value))
        return rows
    # Both Wine .reg files and Windows Registry Editor exports, including
    # wrapped binary values and the 32-bit Wow6432Node key.
    text = re.sub(r'\\\r?\n\s*', '', text)
    section = None
    for line in text.splitlines():
        line = line.strip()
        match = re.match(r'^\[([^\]]+)\]', line)
        if match:
            section = None
            key = normalize_key(match[1])
            root = default_root
            for full, short in [('HKEY_LOCAL_MACHINE', 'HKLM'), ('HKEY_CURRENT_USER', 'HKCU')]:
                if key.upper().startswith(full + '\\'):
                    root, key = short, key[len(full) + 1:]
                    break
            if key.casefold() == KEY.casefold() and root in {'HKLM', 'HKCU'}:
                section = root
            continue
        match = re.match(r'^"([^"\\]+)"=(.*)$', line)
        if not section or not match or match[1] not in ALLOWED:
            continue
        name, raw = match.groups()
        if raw.startswith('"'):
            kind, value = '1', decode_string(raw)
        elif raw.startswith('dword:'):
            try:
                kind, value = '4', str(int(raw[6:], 16))
            except ValueError:
                raise RegistryError('Malformed DWORD in Halo registry export') from None
        elif raw.startswith('hex:'):
            kind, value = '3', raw[4:].replace(',', '').replace(' ', '')
        else:
            continue
        rows.append((section, KEY, name, kind, value))
    return rows


def read_text(path: Path):
    data = path.read_bytes()
    if len(data) > 32 * 1024 * 1024:
        raise RegistryError('Registry file exceeds the setup size limit')
    try:
        return data.decode('utf-16' if data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig')
    except UnicodeError:
        raise RegistryError('Registry export must be UTF-8 or UTF-16') from None


def read_registry(path: Path):
    if path.is_dir():
        rows = []
        for filename, root in [('system.reg', 'HKLM'), ('user.reg', 'HKCU')]:
            file = path / filename
            if file.is_file():
                rows.extend(parse_export(read_text(file), root))
        return finish(rows)
    return finish(parse_export(read_text(path)))
