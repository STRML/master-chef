#!/usr/bin/env python3
"""Read a selected build's live.json over the paired CoreDevice connection.

No listener, new app permission, input injection, or Wi-Fi change. About every
five seconds, fetch only the latest small snapshot; full deep-*.jsonl evidence
stays in the app Documents/Diagnostics folder on the headset. Never accept another
build's evidence. A failed/stale pull is displayed as such, never as live data.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
APP = []


def atomic_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, sort_keys=True) + '\n')
    temporary.replace(path)


def live_candidates(listing):
    candidates = []
    for entry in listing.get('result', {}).get('files', []):
        relative = PurePosixPath(entry.get('relativePath', ''))
        if len(relative.parts) != 2 or relative.name != 'live.json':
            continue
        try:
            uuid.UUID(relative.parts[0])
        except ValueError:
            continue
        candidates.append((entry.get('metadata', {}).get('lastModDate', ''), str(relative)))
    return [path for _, path in sorted(candidates, reverse=True)]


def accepted(record, build_id, build='80'):
    return record.get('buildID') == build_id and str(record.get('build')) == str(build) and bool(record.get('runID'))


def summary(record):
    ages = record.get('layerObservedAgeMilliseconds', [])
    return {
        'build': record.get('build'), 'buildID': record.get('buildID'),
        'budgetTier': record.get('budgetTier'), 'extraHalf': record.get('extraHalf'),
        'runID': record['runID'], 'recordIndex': record.get('recordIndex'),
        'elapsedSeconds': record.get('elapsedSeconds'), 'mode': record.get('mode'),
        'engineFPS': record.get('engineFPS'), 'submittedFPS': record.get('submittedFPS'),
        'maxObservedLayerAgeMs': max(ages, default=-1),
        'audioFramesPerSecond': record.get('audioFramesPerSecond'),
        'audioLateCallbacks': record.get('audioLateCallbacks'),
        'controllerConnected': record.get('controllerConnected'),
        'controllerDisconnects': record.get('controllerDisconnects'),
        'hapticsPlayed': record.get('hapticsPlayed'), 'hapticFailures': record.get('hapticFailures'),
        'activeSignals': record.get('activeSignals', []), 'eventCounts': record.get('eventCounts', {}),
        'writerDroppedRecords': record.get('writerDroppedRecords', 0),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    parser.add_argument('--bundle-id', required=True)
    parser.add_argument('--build', required=True, help='Expected app build number')
    parser.add_argument('--build-id', required=True)
    parser.add_argument('--seconds', type=float, default=900)
    parser.add_argument('--interval', type=float, default=5)
    parser.add_argument('--output', type=Path, default=ROOT / 'native/logs/live')
    args = parser.parse_args()
    global APP
    APP = ['--domain-type', 'appDataContainer', '--domain-identifier', args.bundle_id]
    if not (0 <= args.seconds <= 43200 and 2 <= args.interval <= 60):
        parser.error('seconds must be 0..43200 and interval 2..60')
    args.output.mkdir(parents=True, exist_ok=True)
    lock_file = (args.output / 'watcher.lock').open('w')
    import fcntl
    try:
        fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        parser.error('a watcher already owns this output directory')
    lock_file.write(str(os.getpid())); lock_file.flush()

    def invoke(label, command):
        result_path = args.output / (label + '.json')
        result_path.unlink(missing_ok=True)
        command = ['xcrun', 'devicectl', *command, '--device', args.device,
                   '--timeout', '12', '--json-output', str(result_path)]
        with (args.output / (label + '.log')).open('w') as log:
            try:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=17)
            except subprocess.TimeoutExpired:
                return None
        if result.returncode or not result_path.exists():
            return None
        return json.loads(result_path.read_text())

    deadline = time.monotonic() + args.seconds
    last_list = -float('inf'); selected = None
    previous = None; last_progress = time.monotonic()
    good = 0
    while True:
        start = time.monotonic()
        paths = [selected] if selected else []
        if start - last_list >= 30 or not paths:
            listing = invoke('list', ['device', 'info', 'files', *APP, '--subdirectory', 'Documents/Diagnostics'])
            if listing is not None:
                paths = live_candidates(listing)[:5]
            last_list = start
        record = None
        for path in paths:
            incoming = args.output / 'incoming.json'
            incoming.unlink(missing_ok=True)
            result = invoke('copy', ['device', 'copy', 'from', *APP, '--source', 'Documents/Diagnostics/' + path,
                                     '--destination', str(incoming)])
            if result is None or not incoming.exists():
                continue
            try:
                candidate = json.loads(incoming.read_text())
            except (ValueError, OSError):
                continue
            if accepted(candidate, args.build_id, args.build):
                record = candidate; selected = path; break
        status = {'expectedBuild': args.build, 'expectedBuildID': args.build_id, 'polledAtUnixSeconds': time.time(), 'pid': os.getpid()}
        if record is None:
            status['connection'] = 'unavailable-or-no-matching-build'
            selected = None
        else:
            identity = (record['runID'], record.get('recordIndex'))
            if identity != previous:
                previous = identity; last_progress = time.monotonic()
            # Wall-clock age catches an old run on the very first pull. Also
            # use monotonic progress, independent of clocks on the two devices.
            age = max(0, time.time() - record.get('updatedAtUnixSeconds', 0))
            fresh = age < 15 and time.monotonic() - last_progress < 15
            status.update(connection='live' if fresh else 'stale', snapshotAgeSeconds=age, **summary(record))
            atomic_json(args.output / 'latest-snapshot.json', record)
            good += 1
        atomic_json(args.output / 'status.json', status)
        journal = args.output / 'monitor.jsonl'
        if journal.exists() and journal.stat().st_size > 8 * 1024 * 1024:
            journal.replace(args.output / 'monitor-previous.jsonl')
        with journal.open('a') as stream:
            stream.write(json.dumps(status, sort_keys=True) + '\n')
        print(json.dumps(status, sort_keys=True), flush=True)
        if time.monotonic() >= deadline:
            break
        time.sleep(min(max(0, args.interval - (time.monotonic() - start)), max(0, deadline - time.monotonic())))
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
