#!/usr/bin/env python3
"""Audit release source paths and text; never print suspected secret values.

Use --strict for a distribution tree, including normally ignored artifacts.
This is a layered guard, not a guarantee that every possible secret is detected.
"""
import argparse
from pathlib import Path
import hashlib
import zipfile
from pathlib import PurePosixPath
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SKIP = {'.git', '.build', '.setup', '__pycache__', '.venv', 'game', 'build', 'logs', 'dist', 'DerivedData',
         '.cache', '.scratch', '.gradle'}
# Explicitly reviewed generated documentation art, never a general binary
# allowlist. Changing this image requires reviewing and updating its digest.
DOCUMENTATION_ART = {
    'docs/assets/master-chef-header.png': 'e15bc1102ca36f40e357c39d48149445f94115971a9469406f7e67c9f442a386',
}
BAD_SUFFIX = {'.exe', '.dll', '.map', '.iso', '.ipa', '.p12', '.p8', '.pfx', '.pem', '.key',
              '.mobileprovision', '.provisionprofile', '.o', '.a', '.dylib', '.so', '.pyc',
              '.zip', '.tpf', '.hvt', '.hvs', '.bgra', '.mov', '.mp4', '.jsonl', '.log', '.reg',
              '.keystore', '.jks', '.pk8', '.bik', '.tag'}
RULES = {
    'home-directory': re.compile(r'/(?:Users|home)/[^/\s"\']+'),
    'private-key': re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----'),
    'github-token': re.compile(r'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b'),
    'cloud-key': re.compile(r'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
    'service-token': re.compile(r'\b(?:sk-(?:proj-)?[A-Za-z0-9_-]{30,}|xox[baprs]-[A-Za-z0-9-]{20,})\b'),
    'private-network': re.compile(r'\b(?:192\.168\.\d{1,3}\.\d{1,3}|10\.\d{1,3}\.\d{1,3}\.\d{1,3})\b'),
    'apple-device-id': re.compile(r'\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}\b'),
}
# Quest lane guards. jniLibs staging and the debug APK live under gitignored
# build directories that the source audit skips, so they get an explicit audit:
# only the allowlisted native libraries, only the arm64-v8a ABI, no game
# payload, no signing material. The game payload is adb-pushed to
# app-specific storage at runtime; it must never be packaged.
QUEST_ABIS = {'arm64-v8a'}
QUEST_JNILIBS_ALLOW = {'libhaloquest.so', 'libopenxr.so', 'libc++_shared.so'}
GAME_DATA_SUFFIX = {'.bik', '.map', '.tag', '.wav', '.ogg', '.hlst', '.hmt', '.asb', '.ssf'}
GAME_DATA_NAMES = {'halo.exe', 'strings.dll', 'fx.bin', 'halo-vision-registry.txt', 'setup.txt'}
# Gradle signing inputs; the (X) groups keep this pattern from matching itself.
SIGNING_CONFIG = re.compile(r'\bst(o)re(?:File|Password)\b|\bkey(A)(?:lias|Password)\b')
# Gitignored native build executables (see .gitignore); binaries by design,
# never committable, audited as files only in --strict mode.
GITIGNORED_BUILD_NAMES = {'halo-headless', 'halo-headless-static', 'vk_smoke', 'halo-host'}

def audit(root, strict=False):
    failures=[]; count=0
    for p in sorted(root.rglob('*')):
        rel=p.relative_to(root)
        if '.git' in rel.parts:continue
        if not strict and any(part in SKIP for part in rel.parts):continue
        if p.is_symlink():failures.append((str(rel),0,'symbolic-link'));continue
        if not p.is_file():continue
        if not strict and p.name in GITIGNORED_BUILD_NAMES:continue
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
    return count,failures+audit_quest(root)

def audit_quest(root):
    """Quest lane guards over build staging the source audit skips:
    jniLibs contents, APK zip entries, signing references in gradle files,
    and containment of the game payload at the repository root."""
    failures=[]
    for p in sorted(root.rglob('*')):
        rel=p.relative_to(root)
        if '.git' in rel.parts:continue
        if not p.is_file():continue
        posix=rel.as_posix();parts=rel.parts
        if p.suffix.lower() in {'.keystore','.jks','.pk8'}:
            failures.append((posix,0,'signing-material'))
        if 'game' in parts[1:]:
            failures.append((posix,0,'game-payload-nested'))
        if 'jniLibs' in parts:
            idx=parts.index('jniLibs')
            tail=parts[idx+1:]
            if len(tail)==2:
                if tail[0] not in QUEST_ABIS:failures.append((posix,0,'quest-jnilibs-abi'))
                name=p.name.lower()
                if p.suffix.lower() in GAME_DATA_SUFFIX or p.name in GAME_DATA_NAMES:
                    failures.append((posix,0,'quest-jnilibs-game-data'))
                elif p.suffix.lower()!='.so':failures.append((posix,0,'quest-jnilibs-non-native'))
                elif name not in QUEST_JNILIBS_ALLOW:failures.append((posix,0,'quest-jnilibs-unexpected-lib'))
            elif len(tail)>2:failures.append((posix,0,'quest-jnilibs-nesting'))
        if posix.startswith('android/') and p.suffix.lower() in {'.gradle','.properties','.xml'}:
            try:text=p.read_text(encoding='utf-8')
            except UnicodeError:continue
            for n,line in enumerate(text.splitlines(),1):
                if SIGNING_CONFIG.search(line):failures.append((posix,n,'android-signing-config'))
        if posix.startswith('android/') and p.suffix.lower()=='.apk':
            try:
                with zipfile.ZipFile(p) as apk:entries=apk.namelist()
            except zipfile.BadZipFile:
                failures.append((posix,0,'unreadable-apk'));continue
            for e in entries:
                ep=PurePosixPath(e).parts;efix=PurePosixPath(e).suffix.lower()
                if efix in GAME_DATA_SUFFIX or e in GAME_DATA_NAMES or ep[0]=='assets':
                    failures.append((posix+':'+e,0,'apk-game-data'))
                if len(ep)>1 and ep[0]=='lib' and ep[1] not in QUEST_ABIS:
                    failures.append((posix+':'+e,0,'apk-abi'))
    return failures

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--strict',action='store_true')
    parser.add_argument('--root',type=Path,default=ROOT);args=parser.parse_args()
    count,failures=audit(args.root.resolve(),args.strict)
    for path,line,kind in failures:print(f'{path}:{line}: {kind}')
    print(f'{"FAIL" if failures else "PASS"}: {count} source files checked; {len(failures)} findings')
    return bool(failures)

if __name__=='__main__':sys.exit(main())
