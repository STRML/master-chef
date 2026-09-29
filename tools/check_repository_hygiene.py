#!/usr/bin/env python3
"""Audit release source paths and text; never print suspected secret values.

Use --strict for a distribution tree, including normally ignored artifacts.
This is a layered guard, not a guarantee that every possible secret is detected.
"""
import argparse
import hashlib
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SKIP = {'.git', '.build', '.setup', '__pycache__', '.venv', 'game', 'build', 'logs', 'dist', 'DerivedData'}
# Explicitly reviewed generated documentation art, never a general binary
# allowlist. Changing this image requires reviewing and updating its digest.
DOCUMENTATION_ART = {
    'docs/assets/master-chef-header.png': '93434569597096205b9d213172e8e395195bb3d803bfc27cae0e5e6d37e95139',
}
BAD_SUFFIX = {'.exe', '.dll', '.map', '.iso', '.ipa', '.p12', '.p8', '.pfx', '.pem', '.key',
              '.mobileprovision', '.provisionprofile', '.o', '.a', '.dylib', '.so', '.pyc',
              '.zip', '.tpf', '.hvt', '.hvs', '.bgra', '.mov', '.mp4', '.jsonl', '.log', '.reg'}
RULES = {
    'home-directory': re.compile(r'/(?:Users|home)/[^/\s"\']+'),
    'private-key': re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----'),
    'github-token': re.compile(r'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b'),
    'cloud-key': re.compile(r'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
    'service-token': re.compile(r'\b(?:sk-(?:proj-)?[A-Za-z0-9_-]{30,}|xox[baprs]-[A-Za-z0-9-]{20,})\b'),
    'private-network': re.compile(r'\b(?:192\.168\.\d{1,3}\.\d{1,3}|10\.\d{1,3}\.\d{1,3}\.\d{1,3})\b'),
    'apple-device-id': re.compile(r'\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}\b'),
}

def audit(root, strict=False):
    failures=[]; count=0
    for p in sorted(root.rglob('*')):
        rel=p.relative_to(root)
        if '.git' in rel.parts:continue
        if not strict and any(part in SKIP for part in rel.parts):continue
        if p.is_symlink():failures.append((str(rel),0,'symbolic-link'));continue
        if not p.is_file():continue
        count+=1
        if (p.suffix.lower() in BAD_SUFFIX or p.name in {'.DS_Store','.env','halo-vision-registry.txt'}
            or any(part.endswith(('.app','.xcarchive','.dSYM')) for part in rel.parts)
            or any(part in {'assessment','local-agent-inputs','.context','.claude','.setup'} for part in rel.parts)):
            failures.append((str(rel),0,'non-source-artifact'))
        if rel.as_posix() in DOCUMENTATION_ART:
            data=p.read_bytes()
            if not data.startswith(b'\x89PNG\r\n\x1a\n') or hashlib.sha256(data).hexdigest()!=DOCUMENTATION_ART[rel.as_posix()]:
                failures.append((str(rel),0,'unreviewed-documentation-art'))
            continue
        try:text=p.read_text(encoding='utf-8')
        except UnicodeError:
            failures.append((str(rel),0,'binary-file'));continue
        for n,line in enumerate(text.splitlines(),1):
            for kind,pattern in RULES.items():
                if pattern.search(line):failures.append((str(rel),n,kind))
    return count,failures

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--strict',action='store_true')
    parser.add_argument('--root',type=Path,default=ROOT);args=parser.parse_args()
    count,failures=audit(args.root.resolve(),args.strict)
    for path,line,kind in failures:print(f'{path}:{line}: {kind}')
    print(f'{"FAIL" if failures else "PASS"}: {count} source files checked; {len(failures)} findings')
    return bool(failures)

if __name__=='__main__':sys.exit(main())
