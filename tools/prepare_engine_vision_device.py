#!/usr/bin/env python3
"""Stage, sign, and verify a standalone EngineVision device smoke package.

This tool never contacts or modifies a device. It prepares one signed .app
containing the user's canonical, manifest-verified locally owned Halo files.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import uuid


REPO = Path(__file__).resolve().parents[1]
DEFAULT_APP = REPO / "native/EngineVision/.build/DirectXROS/HaloVision.app"
DEFAULT_PROFILE_APP = None
DEFAULT_GAME = REPO / "game"
DEFAULT_OUTPUT = REPO / "native/EngineVision/.build/device-staging"
ENTITLEMENTS = None
BUNDLE_ID = None
TEAM_ID = None
DEVICE_UDID = None
CORE_DEVICE_ID = None
IDENTITY_SHA1 = None
HALO_SHA256 = "c9acf0c469543283cfed6d7dc04ade976dbdfc7cb4532cf070386de169c19545"
REQUIRED_GAME_FILES = {
    "halo.exe": 2_000_000,
    "maps/ui.map": 2_000_000,
    "maps/a10.map": 90_000_000,
    "maps/bitmaps.map": 300_000_000,
    "maps/sounds.map": 200_000_000,
    "shaders/vsh.bin": 30_000,
    "shaders/fx.bin": 900_000,
    "strings.dll": 1_000_000,
    "halo-vision-registry.txt": 100,
}



def clone_copy(source, destination, *, follow_symlinks=True):
    """Copy on write on APFS to avoid duplicating multi-GB release assets."""
    import ctypes, os
    source, destination = Path(source), Path(destination)
    if destination.is_dir(): destination = destination / source.name
    if sys.platform == 'darwin' and not destination.exists():
        clone = ctypes.CDLL(None, use_errno=True).clonefile
        clone.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        clone.restype = ctypes.c_int
        if clone(os.fsencode(source.resolve()), os.fsencode(destination), 0) == 0:
            shutil.copystat(source, destination, follow_symlinks=follow_symlinks)
            return str(destination)
    return shutil.copy2(source, destination, follow_symlinks=follow_symlinks)

class PreparationError(RuntimeError):
    pass


def run(command: list[str], *, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and result.returncode:
        detail = (result.stderr or result.stdout).decode(errors="replace").strip()
        raise PreparationError(f"command failed ({result.returncode}): {' '.join(command)}\n{detail}")
    return result


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def tree_inventory(root: Path) -> tuple[list[dict[str, object]], int, str]:
    rows: list[dict[str, object]] = []
    total = 0
    for path in sorted((item for item in root.rglob("*") if item.is_file()),
                       key=lambda item: item.relative_to(root).as_posix().lower()):
        relative = path.relative_to(root).as_posix()
        size = path.stat().st_size
        rows.append({"path": relative, "bytes": size, "sha256": sha256_file(path)})
        total += size
    canonical = json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()
    return rows, total, hashlib.sha256(canonical).hexdigest()


def inventory_digest(rows: list[dict[str, object]]) -> str:
    """Hash destination paths and file contents, excluding staging-only metadata."""
    canonical_rows = [
        {"path": str(row["path"]), "bytes": int(row["bytes"]), "sha256": str(row["sha256"])}
        for row in rows
    ]
    canonical = json.dumps(canonical_rows, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(canonical).hexdigest()


def canonical_game_path(raw: str) -> str:
    """Match EngineAssetImporter.canonicalRelativePath exactly."""
    components = raw.split("/")
    if not components:
        return raw
    first = components[0].lower()
    if first in {"maps", "shaders"}:
        components[0] = first
    elif len(components) == 1:
        components[0] = first
    return "/".join(components)


def directory_size(root: Path) -> int:
    return sum(item.stat().st_size for item in root.rglob("*") if item.is_file())


def plist_from_codesign(app: Path) -> dict[str, object]:
    result = run(["/usr/bin/codesign", "-d", "--entitlements", ":-", str(app)])
    payload = result.stdout or result.stderr
    start = payload.find(b"<?xml")
    if start < 0:
        start = payload.find(b"<plist")
    if start < 0:
        raise PreparationError("codesign did not return an entitlement plist")
    return plistlib.loads(payload[start:])


def decode_profile(profile_path: Path) -> dict[str, object]:
    result = run(["/usr/bin/security", "cms", "-D", "-i", str(profile_path)])
    return plistlib.loads(result.stdout)


def validate_app(app: Path) -> dict[str, object]:
    if not app.is_dir():
        raise PreparationError(f"input app is missing: {app}")
    info_path = app / "Info.plist"
    if not info_path.is_file():
        raise PreparationError(f"input app has no Info.plist: {app}")
    info = plistlib.loads(info_path.read_bytes())
    if info.get("CFBundleIdentifier") != BUNDLE_ID:
        raise PreparationError(f"input bundle ID must be {BUNDLE_ID}")
    if "XROS" not in info.get("CFBundleSupportedPlatforms", []):
        raise PreparationError("input app does not declare XROS support")
    executable_name = info.get("CFBundleExecutable")
    executable = app / str(executable_name)
    if not executable.is_file():
        raise PreparationError("input app executable is missing")
    file_output = run(["/usr/bin/file", str(executable)]).stdout.decode(errors="replace")
    if "Mach-O 64-bit executable arm64" not in file_output:
        raise PreparationError(f"input executable is not arm64 Mach-O: {file_output.strip()}")
    load_commands = run(["/usr/bin/otool", "-l", str(executable)]).stdout.decode(errors="replace")
    if "platform 11" not in load_commands:
        raise PreparationError("input executable has no visionOS LC_BUILD_VERSION (platform 11)")
    return {
        "bundle_id": BUNDLE_ID,
        "executable": executable_name,
        "executable_sha256": sha256_file(executable),
        "bundle_bytes": directory_size(app),
        "minimum_os": info.get("MinimumOSVersion"),
    }


def validate_profile_and_identity(profile_app: Path) -> tuple[Path, dict[str, object]]:
    profile_path = profile_app / "embedded.mobileprovision"
    if not profile_path.is_file():
        raise PreparationError(f"reference provisioning profile is missing: {profile_path}")
    profile = decode_profile(profile_path)
    now = dt.datetime.now(dt.timezone.utc)
    expiration = profile.get("ExpirationDate")
    if isinstance(expiration, dt.datetime):
        if expiration.tzinfo is None:
            expiration = expiration.replace(tzinfo=dt.timezone.utc)
        if expiration <= now:
            raise PreparationError(f"provisioning profile expired at {expiration.isoformat()}")
    if TEAM_ID not in profile.get("TeamIdentifier", []):
        raise PreparationError("provisioning profile has the wrong team")
    if DEVICE_UDID not in profile.get("ProvisionedDevices", []):
        raise PreparationError("provisioning profile does not contain the Vision Pro UDID")
    platforms = profile.get("Platform", [])
    if not any(str(item).lower() in {"xros", "visionos"} for item in platforms):
        raise PreparationError("provisioning profile does not include xrOS/visionOS")
    allowed_app_id = profile.get("Entitlements", {}).get("application-identifier")
    if allowed_app_id not in {f"{TEAM_ID}.*", f"{TEAM_ID}.{BUNDLE_ID}"}:
        raise PreparationError("provisioning profile does not allow the target application identifier")

    certificates = profile.get("DeveloperCertificates", [])
    profile_fingerprints = {hashlib.sha1(bytes(item)).hexdigest().upper() for item in certificates}
    if IDENTITY_SHA1 not in profile_fingerprints:
        raise PreparationError("the selected signing identity is not included in the profile")
    identities = run(["/usr/bin/security", "find-identity", "-v", "-p", "codesigning"]).stdout.decode(errors="replace")
    usable = set(re.findall(r'^\s*\d+\)\s+([0-9A-Fa-f]{40})\s+"[^"\n]+"\s*$', identities, re.MULTILINE))
    if IDENTITY_SHA1 not in {fingerprint.upper() for fingerprint in usable}:
        raise PreparationError("the required Apple Development signing identity is unavailable")

    # Use the exact selected certificate, not a name shared by multiple accounts.
    certificate_der = next(bytes(item) for item in certificates
                           if hashlib.sha1(bytes(item)).hexdigest().upper() == IDENTITY_SHA1)
    with tempfile.NamedTemporaryFile(suffix=".der") as cert_file:
        cert_file.write(certificate_der)
        cert_file.flush()
        cert_dates = run(["/usr/bin/openssl", "x509", "-inform", "DER", "-in", cert_file.name,
                          "-noout", "-dates"]).stdout.decode().strip()

    return profile_path, {
        "name": profile.get("Name"),
        "uuid": profile.get("UUID"),
        "team_id": TEAM_ID,
        "application_identifier": allowed_app_id,
        "created_at": profile.get("CreationDate").isoformat() if profile.get("CreationDate") else None,
        "expires_at": expiration.isoformat() if isinstance(expiration, dt.datetime) else None,
        "platforms": platforms,
        "provisioned_devices": profile.get("ProvisionedDevices", []),
        "identity_sha1": IDENTITY_SHA1,
        "identity_dates": cert_dates.splitlines(),
    }


def validate_entitlements() -> dict[str, object]:
    entitlements = plistlib.loads(ENTITLEMENTS.read_bytes())
    if entitlements.get("application-identifier") != f"{TEAM_ID}.{BUNDLE_ID}":
        raise PreparationError("staging entitlements contain the wrong application identifier")
    if entitlements.get("com.apple.developer.team-identifier") != TEAM_ID:
        raise PreparationError("staging entitlements contain the wrong team")
    return entitlements


def validate_game(game: Path) -> tuple[Path, list[dict[str, object]], int, str]:
    root = game.resolve(strict=True)
    source_rows, total, _ = tree_inventory(root)
    rows: list[dict[str, object]] = []
    destinations: set[str] = set()
    for source_row in source_rows:
        source_path = str(source_row["path"])
        destination_path = canonical_game_path(source_path)
        destination_key = destination_path.lower()
        if destination_key in destinations:
            raise PreparationError(f"owned game folder has two files mapping to {destination_path}")
        destinations.add(destination_key)
        rows.append({**source_row, "source_path": source_path, "path": destination_path})
    rows.sort(key=lambda row: (str(row["path"]).lower(), str(row["path"])))
    tree_hash = inventory_digest(rows)
    lookup = {str(row["path"]).lower(): row for row in rows}
    for relative, minimum in REQUIRED_GAME_FILES.items():
        row = lookup.get(relative)
        if row is None:
            raise PreparationError(f"owned game folder is missing {relative}")
        if int(row["bytes"]) < minimum:
            raise PreparationError(f"owned game file is too small: {relative}")
    if lookup["halo.exe"]["sha256"] != HALO_SHA256:
        raise PreparationError("owned halo.exe SHA-256 does not match PC 1.10")
    return root, rows, total, tree_hash


def manifest_rows(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    return [{"path": str(row["path"]), "bytes": int(row["bytes"]),
             "sha256": str(row["sha256"])} for row in rows]


def validate_bundled_game_payload(app: Path) -> dict[str, object]:
    manifest_path = app / "GamePayloadManifest.json"
    payload_root = app / "GamePayload"
    if (not manifest_path.is_file() or manifest_path.is_symlink() or
            not payload_root.is_dir() or payload_root.is_symlink()):
        raise PreparationError("signed app is missing GamePayload or GamePayloadManifest.json")
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict) or manifest.get("formatVersion") != 1:
        raise PreparationError("GamePayload manifest formatVersion must be 1")
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise PreparationError("GamePayload manifest has no files")
    normalized: list[dict[str, object]] = []
    destinations: set[str] = set()
    for row in files:
        if not isinstance(row, dict):
            raise PreparationError("GamePayload manifest contains a non-object file entry")
        relative = str(row.get("path", ""))
        path = Path(relative)
        if not relative or path.is_absolute() or ".." in path.parts or relative == "engine-vision-import.json":
            raise PreparationError(f"GamePayload manifest has an unsafe or forbidden path: {relative}")
        key = relative.lower()
        if key in destinations:
            raise PreparationError(f"GamePayload manifest has a duplicate destination: {relative}")
        destinations.add(key)
        file_path = payload_root / path
        if not file_path.is_file() or file_path.is_symlink():
            raise PreparationError(f"GamePayload file is missing or symbolic: {relative}")
        size = file_path.stat().st_size
        if not isinstance(row.get("bytes"), int) or int(row["bytes"]) != size:
            raise PreparationError(f"GamePayload byte length differs from manifest: {relative}")
        digest = sha256_file(file_path)
        if row.get("sha256") != digest:
            raise PreparationError(f"GamePayload SHA-256 differs from manifest: {relative}")
        normalized.append({"path": relative, "bytes": size, "sha256": digest})
    normalized.sort(key=lambda row: (str(row["path"]).lower(), str(row["path"])))
    actual, total, _ = tree_inventory(payload_root)
    if actual != normalized:
        raise PreparationError("GamePayload contains files absent from its exact manifest")
    payload_id = inventory_digest(normalized)
    if manifest.get("payloadID") != payload_id:
        raise PreparationError("GamePayload payloadID differs from its canonical inventory")
    if manifest.get("fileCount") != len(normalized) or manifest.get("totalBytes") != total:
        raise PreparationError("GamePayload manifest count or total bytes is wrong")
    if manifest.get("executableSHA256") != HALO_SHA256:
        raise PreparationError("GamePayload manifest has the wrong original executable SHA-256")
    lookup = {str(row["path"]): row for row in normalized}
    for relative, minimum in REQUIRED_GAME_FILES.items():
        if relative not in lookup or int(lookup[relative]["bytes"]) < minimum:
            raise PreparationError(f"GamePayload required file is absent or too small: {relative}")
    return {"manifest": str(manifest_path), "payload_id": payload_id,
            "file_count": len(normalized), "total_bytes": total,
            "executable_sha256": HALO_SHA256, "files": normalized}


def stage_and_sign_app(source: Path, destination: Path, profile_path: Path,
                       game_source: Path, game_rows: list[dict[str, object]],
                       game_total: int, game_tree_hash: str) -> dict[str, object]:
    temporary = destination.parent / f".HaloVision-staging-{uuid.uuid4().hex}.app"
    if temporary.exists():
        shutil.rmtree(temporary)
    shutil.copytree(source, temporary, symlinks=False, copy_function=clone_copy)
    payload_root = temporary / "GamePayload"
    if payload_root.exists():
        shutil.rmtree(payload_root)
    payload_root.mkdir()
    clean_rows = manifest_rows(game_rows)
    for row in game_rows:
        relative = Path(str(row["path"]))
        source_file = game_source / Path(str(row.get("source_path", row["path"])))
        target = payload_root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        clone_copy(source_file, target, follow_symlinks=True)
    manifest = {"formatVersion": 1, "payloadID": game_tree_hash,
                "fileCount": len(clean_rows), "totalBytes": game_total,
                "executableSHA256": HALO_SHA256, "files": clean_rows}
    write_json_atomic(temporary / "GamePayloadManifest.json", manifest)
    payload = validate_bundled_game_payload(temporary)
    if payload["payload_id"] != game_tree_hash:
        raise PreparationError("bundled GamePayload does not match the validated source inventory")
    clone_copy(profile_path, temporary / "embedded.mobileprovision")
    run(["/usr/bin/codesign", "--force", "--sign", IDENTITY_SHA1,
         "--entitlements", str(ENTITLEMENTS), "--generate-entitlement-der",
         "--timestamp=none", str(temporary)])
    run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=4", str(temporary)])
    signed = plist_from_codesign(temporary)
    if signed.get("application-identifier") != f"{TEAM_ID}.{BUNDLE_ID}":
        raise PreparationError("signed app has the wrong application identifier")
    if signed.get("com.apple.developer.team-identifier") != TEAM_ID:
        raise PreparationError("signed app has the wrong team identifier")
    payload = validate_bundled_game_payload(temporary)
    info = plistlib.loads((temporary / "Info.plist").read_bytes())
    executable = temporary / str(info["CFBundleExecutable"])
    rows, total, tree_hash = tree_inventory(temporary)
    if destination.exists():
        shutil.rmtree(destination)
    temporary.replace(destination)
    payload["manifest"] = str(destination / "GamePayloadManifest.json")
    return {
        "path": str(destination),
        "executable_sha256": sha256_file(destination / executable.name),
        "bundle_tree_sha256": tree_hash,
        "bundle_bytes": total,
        "file_count": len(rows),
        "signed_entitlements": signed,
        "game_payload": payload,
    }


def write_json_atomic(path: Path, payload: object) -> None:
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True, default=str) + "\n")
    temporary.replace(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True,
                        help="rebuilt unsigned EngineVision .app to stage")
    parser.add_argument("--profile-app", type=Path, required=True,
                        help="existing signed app that supplies the valid provisioning profile")
    parser.add_argument("--game", type=Path, default=DEFAULT_GAME,
                        help="owned Halo game folder (symlink roots are resolved)")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                        help="device package staging directory")
    parser.add_argument("--identity-sha1", required=True,
                        help="40-hex signing identity fingerprint available in this account")
    parser.add_argument("--entitlements", type=Path, required=True,
                        help="signing entitlement plist on this build machine")
    parser.add_argument("--preflight-only", action="store_true",
                        help="verify app, profile, identity, entitlements, disk, and game without staging")
    parser.add_argument("--team-id", required=True)
    parser.add_argument("--bundle-id", required=True)
    parser.add_argument("--device-udid", required=True)
    parser.add_argument("--core-device-id", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9A-Fa-f]{40}", args.identity_sha1):
        parser.error("--identity-sha1 must be exactly 40 hexadecimal characters")
    args.identity_sha1 = args.identity_sha1.upper()
    return args


def main() -> int:
    global IDENTITY_SHA1, ENTITLEMENTS, TEAM_ID, BUNDLE_ID, DEVICE_UDID, CORE_DEVICE_ID
    args = parse_args()
    TEAM_ID, BUNDLE_ID = args.team_id, args.bundle_id
    DEVICE_UDID, CORE_DEVICE_ID = args.device_udid, args.core_device_id
    IDENTITY_SHA1 = args.identity_sha1
    ENTITLEMENTS = args.entitlements.resolve(strict=True)
    source_app = args.app.resolve(strict=True)
    profile_app = args.profile_app.resolve(strict=True)
    app = validate_app(source_app)
    profile_path, profile = validate_profile_and_identity(profile_app)
    entitlements = validate_entitlements()
    game_root, game_rows, game_total, game_tree_hash = validate_game(args.game)
    required = game_total + int(app["bundle_bytes"]) + 2 * 1024 * 1024 * 1024
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists():
        raise PreparationError("output already exists; choose a fresh staging directory")
    free = shutil.disk_usage(args.output.parent).free
    if free < required:
        raise PreparationError(f"insufficient free disk: need at least {required} bytes, have {free}")

    common = {
        "prepared_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "mode": "preflight-only" if args.preflight_only else "complete",
        "source_app": {"path": str(source_app), **app},
        "profile": profile,
        "entitlements": entitlements,
        "device": {"provisioned_udid": DEVICE_UDID, "coredevice_identifier": CORE_DEVICE_ID},
        "game_source": {"path": str(game_root), "file_count": len(game_rows),
                        "total_bytes": game_total, "tree_sha256": game_tree_hash},
        "disk_free_bytes_before": free,
        "device_contacted": False,
        "device_install_or_launch_performed": False,
    }
    if args.preflight_only:
        print(json.dumps(common, indent=2, sort_keys=True, default=str))
        return 0

    args.output.mkdir(parents=True, exist_ok=True)
    common["signed_app"] = stage_and_sign_app(source_app, args.output / "HaloVision.app", profile_path,
                                                game_root, game_rows, game_total, game_tree_hash)
    legacy_payload = args.output / "app-data"
    if legacy_payload.exists():
        shutil.rmtree(legacy_payload)
    common["disk_free_bytes_after"] = shutil.disk_usage(args.output.parent).free
    write_json_atomic(args.output / "preparation.json", common)
    print(json.dumps({"status": "prepared", "mode": common["mode"],
                      "receipt": str(args.output / "preparation.json"),
                      "signed_app": common.get("signed_app", {}).get("path"),
                      "game_bytes": common["signed_app"]["game_payload"]["total_bytes"],
                      "game_files": common["signed_app"]["game_payload"]["file_count"]}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (PreparationError, OSError, ValueError, plistlib.InvalidFileException) as error:
        print(f"device preparation failed: {error}", file=sys.stderr)
        sys.exit(1)
