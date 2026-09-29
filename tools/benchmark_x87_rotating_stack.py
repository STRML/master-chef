#!/usr/bin/env python3
"""Compare and time real translated leaves against both x87 stack layouts.

Generated input is read only. Builds live in an automatically removed temporary
directory; no game/probe/device execution occurs. Timings are local-host results.
Use --check-only in source checks. Supply --generated or HALO_GENERATED_ENGINE
when the generated tree is outside this checkout or its main git worktree.
"""
import argparse
import hashlib
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "native/EngineHost/tests"
FUNCTIONS = ("sub_0050D5B0", "sub_004CC0D0", "sub_00554260", "sub_00553380")


def generated_engine(explicit=None):
    if explicit:
        candidates = [Path(explicit)]
    else:
        roots = [ROOT]
        try:
            common = subprocess.check_output(
                ["git", "rev-parse", "--git-common-dir"], cwd=ROOT, text=True
            ).strip()
            roots.append((ROOT / common).resolve().parent)
        except (OSError, subprocess.CalledProcessError):
            pass
        candidates = [Path(os.environ[k]) for k in ("HALO_GENERATED_ENGINE", "HALO_ENGINE_GEN") if os.environ.get(k)]
        candidates += [root / "native/build/engine-reuse/whole-exe" for root in roots]
    return next((p for p in candidates if all((p / f"{name}.c").is_file() for name in FUNCTIONS)), None)


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def run(command):
    print("+", shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, check=True, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=Path)
    parser.add_argument("--check-only", action="store_true", help="run equivalence without timing")
    parser.add_argument("--allow-missing", action="store_true", help="explicit SKIP if no generated tree exists")
    parser.add_argument("--sanitize", action="store_true", help="ASan/UBSan correctness run; disables timing")
    parser.add_argument("--iterations", type=positive, default=20000)
    parser.add_argument("--samples", type=positive, default=9, help="odd number of alternating-order timing pairs")
    args = parser.parse_args()
    if not args.samples & 1:
        parser.error("--samples must be odd")
    generated = generated_engine(args.generated)
    if generated is None:
        message = "generated C missing; supply --generated or HALO_GENERATED_ENGINE"
        if args.allow_missing:
            print("SKIP: x87 translated comparison:", message)
            return
        parser.error(message)
    cc = shlex.split(os.environ.get("CC", "clang"))
    print("Host:", platform.platform(), platform.machine(), flush=True)
    print("Compiler:", subprocess.check_output([*cc, "--version"], text=True).splitlines()[0], flush=True)
    print("Generated:", generated, flush=True)
    for name in FUNCTIONS:
        digest = hashlib.sha256((generated / f"{name}.c").read_bytes()).hexdigest()
        print(f"SHA256 {name}.c {digest}", flush=True)
    for header in (ROOT / "native/EngineReuse/engine_cpu.h", TESTS / "x87_shifting_engine_cpu.h"):
        print(f"SHA256 {header.relative_to(ROOT)} {hashlib.sha256(header.read_bytes()).hexdigest()}", flush=True)
    flags = (["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
             if args.sanitize else ["-O2"])
    with tempfile.TemporaryDirectory(prefix="halo-x87-translated-") as directory:
        binary = Path(directory) / "x87-translated"
        run([*cc, *flags, "-DENGINE_FLAT_MEMORY=1", "-DHALO_ARM64_FENV_FAST=1",
             "-frounding-math", "-ffp-contract=off", "-I", ROOT / "native/EngineReuse",
             "-I", generated, TESTS / "x87_translated_test.c", TESTS / "x87_translated_shifting.c",
             TESTS / "x87_translated_rotating.c", "-lm", "-o", binary])
        run([binary, 0 if args.check_only or args.sanitize else args.iterations, args.samples])


if __name__ == "__main__":
    main()
