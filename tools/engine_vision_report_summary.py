#!/usr/bin/env python3
"""Summarise a headset diagnostics run: play-time fps, passes, pass cost,
(Build75+) engine-thread CPU time against wall time and core residency, and
audio: silent stretches in play and (Build76+) whether sounds were asked for,
found voices, or waited on the sound cache.
(Build76+) how far apart each panorama layer's camera was from the newest
centre's, and how far layer alignment turned it when it was on.
Core telemetry adds measured frame splits, game ticks, process clocks and
core shares. Tick bucket fits describe association, not isolated tick cost.

usage: python3 tools/engine_vision_report_summary.py native/logs/engine-vision-reports/<stamp>/Diagnostics/<runID> [more run dirs...]"""
import json, math, sys, pathlib, statistics


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)

def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))] if xs else float('nan')

def audio_summary(rows):
    """Silent stretches in play (frames advancing, no nonzero sample), and
    what the engine's sound system was doing in them."""
    live = lambda r: r.get('mode') == 'panorama' and not r.get('menuActive')
    silent, stretches, start, prev = [], [], None, None
    for r in rows:
        if prev and live(r) and live(prev) and r.get('audioFrames', 0) > prev.get('audioFrames', 0):
            quiet = r.get('audioNonzeroSamples') == prev.get('audioNonzeroSamples')
            if quiet:
                silent.append((prev, r))
                start = start if start is not None else prev['elapsedSeconds']
            elif start is not None:
                stretches.append((start, prev['elapsedSeconds'])); start = None
        elif start is not None:
            stretches.append((start, prev['elapsedSeconds'])); start = None
        prev = r
    if start is not None and prev: stretches.append((start, prev['elapsedSeconds']))
    seconds = sum(b['elapsedSeconds'] - a['elapsedSeconds'] for a, b in silent)
    longest = max(stretches, key=lambda x: x[1] - x[0], default=None)
    line = f'  audio   silent in play {seconds:5.1f} s in {len(stretches)} stretches'
    if longest: line += f'; longest {longest[1] - longest[0]:.1f} s at elapsed {longest[0]:.0f}-{longest[1]:.0f}'
    print(line)
    if not any('audioVoicePlays' in r for r in rows):
        print('          (no voice counters before Build76: silences cannot be attributed from the report)'); return
    def rate(key, pairs):
        xs = [(b.get(key, 0) - a.get(key, 0)) / (b['elapsedSeconds'] - a['elapsedSeconds'])
              for a, b in pairs if b['elapsedSeconds'] > a['elapsedSeconds'] and key in a and key in b]
        return statistics.median(xs) if xs else float('nan')
    play = [(a, b) for a, b in zip(rows, rows[1:]) if live(a) and live(b)]
    print(f'          voice starts/s in play median {rate("audioVoicePlays", play):.2f}; in silence {rate("audioVoicePlays", silent):.2f}')
    in_silence = [b for _, b in silent] or [b for _, b in play]
    def span(key):
        xs = [r[key] for r in in_silence if isinstance(r.get(key), (int, float)) and r[key] >= 0]
        return f'{min(xs)}-{max(xs)}' if xs else 'n/a'
    held = sum(b['elapsedSeconds'] - a['elapsedSeconds'] for a, b in play
               if b.get('soundVoicesHeld', 0) > 0 and b.get('soundVoicesFree', 1) == 0)
    print(f'          during silence: sources {span("soundSources")}, channels busy {span("soundChannelsBusy")}, '
          f'voices free {span("soundVoicesFree")}, held {span("soundVoicesHeld")}, cache sounds {span("soundCacheSounds")}, '
          f'sound reads queued {span("soundReadsQueuedSounds")}')
    print(f'          seconds in play with voices held and none free (voice exhaustion): {held:.1f}')
    sounds_reads = rate('soundsMapReads', play)
    if sounds_reads == sounds_reads: print(f'          sounds.map reads/s in play median {sounds_reads:.2f}')

def flatten(r):
    """Timeline arrays as scalar fields the audio summary can range over."""
    q = r.get('soundReadsQueued')
    if isinstance(q, list) and len(q) == 3: r['soundReadsQueuedSounds'] = q[2]
    c = r.get('cacheFileReads')
    if isinstance(c, list) and len(c) == 4: r['soundsMapReads'] = c[2]
    return r

def available_group(record, group):
    values = record.get(group, {})
    if not isinstance(values, dict):
        return {}
    if values.get('available') is False or values.get('snapshotAvailable') is False:
        return {}
    return values


def delta(r, prev, group, key):
    """Difference valid cumulative counters; missing/reset counters are unknown.

    Older reports omit availability flags. Their present counters remain usable.
    A current failed sample must never turn stale/zero values into measurements.
    """
    a, b = available_group(r, group).get(key), available_group(prev, group).get(key)
    def difference(x, y):
        if not number(x) or not number(y) or x < 0 or y < 0 or x < y:
            return None
        return x - y
    if isinstance(a, list):
        if not isinstance(b, list) or len(a) != len(b):
            return None
        values = [difference(x, y) for x, y in zip(a, b)]
        return values if all(v is not None for v in values) else None
    return difference(a, b)


def ratio(numerator, denominator, scale=1):
    if not number(numerator) or not number(denominator) or denominator <= 0:
        return None
    value = scale * numerator / denominator
    return value if math.isfinite(value) else None


def tick_fit(frames, other_seconds, ticks):
    """Frame-weighted fit of residual milliseconds to tick-bucket means.

    Residual excludes pass, limiter idle and guest wait estimates. Bucket 4+
    uses its measured mean tick count; grouping cannot establish tick causation.
    """
    if len(frames) != 5 or len(other_seconds) != 5:
        return None
    if not all(number(x) and x >= 0 for x in [*frames, *other_seconds, ticks]):
        return None
    known = sum(k * frames[k] for k in range(4))
    if ticks < known + 4 * frames[4] or (not frames[4] and ticks != known):
        return None
    xs = [0.0, 1.0, 2.0, 3.0, (ticks - known) / frames[4] if frames[4] else 4.0]
    rows = [(xs[k], other_seconds[k] / frames[k] * 1000, frames[k]) for k in range(5) if frames[k]]
    weight = sum(f for _, _, f in rows)
    if len(rows) < 2 or not weight:
        return None
    mean_x = sum(x * f for x, _, f in rows) / weight
    mean_y = sum(y * f for _, y, f in rows) / weight
    sxx = sum(f * (x - mean_x) ** 2 for x, _, f in rows)
    if sxx <= 0:
        return None
    slope = sum(f * (x - mean_x) * (y - mean_y) for x, y, f in rows) / sxx
    return mean_y - slope * mean_x, slope, rows


def core_summary(play_pairs, report):
    """play_pairs contains (record, previous record) for each play interval."""
    rows = []
    for r, prev in play_pairs:
        n = delta(r, prev, 'engineFrameSplit', 'frames')
        if n is None or n <= 0:
            n = None
            if not all(available_group(r0, 'processCores') for r0 in (r, prev)):
                continue
        get = lambda key, group='engineFrameSplit': delta(r, prev, group, key)
        wall, passes = get('wallSeconds'), get('passSeconds')
        outside = get('outsidePassSeconds')
        # Compatibility for early telemetry reports that only have the split.
        if (outside is None and 'outsidePassSeconds' not in r.get('engineFrameSplit', {})
                and 'outsidePassSeconds' not in prev.get('engineFrameSplit', {})
                and wall is not None and passes is not None and wall >= passes):
            outside = wall - passes
        pcpu, pp = get('cpuSeconds', 'processCores'), get('performanceSeconds', 'processCores')
        pcycles, ppc = get('cycles', 'processCores'), get('performanceCycles', 'processCores')
        pins = get('instructions', 'processCores')
        prun = get('runnableSeconds', 'processCores')
        dt = r['elapsedSeconds'] - prev['elapsedSeconds']
        buckets = get('tickFrames')
        unknown = get('tickUnknownFrames')
        known_frames = (sum(buckets) if buckets is not None and len(buckets) == 5
                        else n - unknown if n is not None and unknown is not None else n)
        counts_available = all(r0.get('engineFrameSplit', {}).get('countsAvailable') is not False for r0 in (r, prev))
        if any('countsFrames' in r0.get('engineFrameSplit', {}) for r0 in (r, prev)):
            counts_available = counts_available and get('countsFrames') == n
        switches_available = all(r0.get('processCores', {}).get('clusterSwitchesAvailable') is not False for r0 in (r, prev))
        rows.append(dict(
            frame_ms=ratio(wall, n, 1000), pass_ms=ratio(passes, n, 1000), outside_ms=ratio(outside, n, 1000),
            idle_ms=ratio(get('idleSeconds'), n, 1000), wait_ms=ratio(get('waitSeconds'), n, 1000),
            other_ms=ratio(get('otherSeconds'), n, 1000), ticks=ratio(get('ticks'), known_frames),
            cpu_wall=ratio(get('cpuSeconds'), wall) if counts_available else None,
            unaccounted_ms=ratio(get('unaccountedOffCoreSeconds'), n, 1000) if counts_available else None,
            process_wait=ratio(max(prun - pcpu, 0), dt) if prun is not None and pcpu is not None else None,
            process_p_share=ratio(pp, pcpu),
            process_ghz=ratio(pcycles, pcpu, 1e-9) if pcycles is not None and pcycles > 0 else None,
            process_p_ghz=ratio(ppc, pp, 1e-9) if ppc is not None and ppc > 0 else None,
            process_ipc=ratio(pins, pcycles) if pins is not None and pins > 0 else None,
            switches=ratio(get('clusterSwitches', 'processCores'), dt) if switches_available else None))
    if rows:
        print('  -- core telemetry: frame splits omit stalls; process counters span whole intervals --')
        for key, label, fmt in (
                ('frame_ms', 'frame ms', '7.2f'), ('pass_ms', 'passes ms/frame', '7.2f'),
                ('outside_ms', 'outside passes ms/fr', '7.2f'), ('idle_ms', 'limiter idle ms/fr', '7.2f'),
                ('wait_ms', 'guest waits ms/fr', '7.2f'), ('other_ms', 'residual ms/frame', '7.2f'),
                ('ticks', 'ticks/known frame', '7.2f'), ('cpu_wall', 'engine CPU/wall', '7.2f'),
                ('unaccounted_ms', 'off-core unacc ms', '7.2f'), ('process_wait', 'proc queued cores', '7.2f'),
                ('process_p_share', 'proc P share', '7.2f'), ('process_ghz', 'proc effective GHz', '7.2f'),
                ('process_p_ghz', 'proc P effective GHz', '7.2f'), ('process_ipc', 'proc IPC', '7.2f'),
                ('switches', 'P/E switches/s', '7.1f')):
            xs = [x[key] for x in rows if x[key] is not None]
            if xs:
                print(f'  {label:20s} median {statistics.median(xs):{fmt}}  p10 {pct(xs, .1):{fmt}}  p90 {pct(xs, .9):{fmt}}')
        print('  residual excludes bearing passes, limiter idle and guest wait estimates; effective GHz = cycles / CPU time')
        if any(row['process_wait'] is not None for row in rows):
            print('  proc queued cores = whole-process max(runnable time - CPU time, 0) / wall time')
    frames, other, ticks = [0] * 5, [0.0] * 5, 0
    for r, prev in play_pairs:
        f = delta(r, prev, 'engineFrameSplit', 'tickFrames')
        o = delta(r, prev, 'engineFrameSplit', 'tickOtherSeconds')
        t = delta(r, prev, 'engineFrameSplit', 'ticks')
        if f is not None and o is not None and len(f) == len(o) == 5 and t is not None:
            frames = [a + b for a, b in zip(frames, f)]
            other = [a + b for a, b in zip(other, o)]
            ticks += t
    fit = tick_fit(frames, other, ticks)
    if fit:
        fixed, per_tick, buckets = fit
        print('  frames by known ticks: ' + '  '.join(f'{x:.1f} ticks: {f} fr, {y:.1f} residual ms' for x, y, f in buckets))
        print(f'  residual = {fixed:.1f} ms + {per_tick:.1f} ms per tick (weighted bucket fit; association, not isolated tick cost)')
    split = available_group(report, 'engineFrameSplit')
    details = []
    if number(split.get('stallFrames')) and number(split.get('stallSeconds')):
        details.append(f'{split["stallFrames"]} frames over 1 s ({split["stallSeconds"]:.1f} s)')
    for key, label in (('tickResyncs', 'tick resyncs'), ('tickUnknownFrames', 'frames with unknown ticks'),
                       ('tickMismatchFrames', 'frames differing from last tick-driver call'),
                       ('maxTicksPerFrame', 'most known ticks in a frame')):
        if number(split.get(key)):
            details.append(f'{label} {split[key]}')
    if details:
        print('  whole run: ' + ', '.join(details))
    process = available_group(report, 'processCores')
    qos = process.get('qosSeconds')
    if isinstance(qos, dict) and all(number(v) and v >= 0 for v in qos.values()) and sum(qos.values()) > 0:
        total = sum(qos.values())
        print('  process CPU by applied QoS: ' + ' '.join(f'{k} {100 * v / total:.0f}%' for k, v in qos.items() if v))
    sched = available_group(report, 'engineThreadScheduling')
    if all(number(sched.get(key)) for key in ('policy', 'priority', 'basePriority', 'cpuUsagePercent')):
        print(f'  engine thread now: policy {sched["policy"]} priority {sched["priority"]} '
              f'(base {sched["basePriority"]}), kernel CPU usage {sched["cpuUsagePercent"]}%')



def main(paths):
    for d in map(pathlib.Path, paths):
        rows = []
        for f in sorted(d.glob('timeline-*.jsonl')):
            rows += [flatten(json.loads(l)) for l in f.read_text().splitlines() if l.strip()]
        report = json.loads((d / 'report.json').read_text()) if (d / 'report.json').exists() else {}
        play, pairs, prev = [], [], None
        for r in rows:
            if prev and r.get('mode') == 'panorama' and not r.get('menuActive') and r.get('intervalSeconds', 0) > 0.5:
                dt = r['elapsedSeconds'] - prev['elapsedSeconds']
                frames = r['frameSequence'] - prev['frameSequence']
                passes = r['enginePasses'] - prev['enginePasses']
                pass_s = r['enginePassSeconds'] - prev['enginePassSeconds']
                current_cpu, previous_cpu = r.get('engineThreadCPUSeconds'), prev.get('engineThreadCPUSeconds')
                cpu = (current_cpu - previous_cpu if number(current_cpu) and number(previous_cpu)
                       and current_cpu >= previous_cpu else None)
                # Build76+: host work outside the passes, per engine frame.
                def per_frame(key, scale=1.0):
                    return scale * (r[key] - prev[key]) / frames if key in r and key in prev and frames > 0 else None
                if frames > 0 and dt > 0:
                    play.append(dict(t=r['elapsedSeconds'], fps=frames / dt, ppf=passes / frames,
                                     ms_pass=1000 * pass_s / passes if passes else float('nan'),
                                     pass_share=pass_s / dt, tier=r.get('panoramaBudgetTier'),
                                     busy=r.get('panoramaBusyMilliseconds'), cpu=cpu / dt if cpu is not None else None,
                                     thermal=r.get('thermalState'),
                                     captures=per_frame('controllerCaptures'), reuses=per_frame('controllerReadingsReused'),
                                     cache_waits=per_frame('engineCacheWaits'), cache_wait_ms=per_frame('engineCacheWaitSeconds', 1000),
                                     reads=per_frame('cacheReaderReads'), read_ms=per_frame('cacheReaderReadSeconds', 1000)))
                    pairs.append((r, prev))
            prev = r
        print(f'== {d.name}  build {rows[0].get("build") if rows else "?"}  samples {len(rows)}  play intervals {len(play)}')
        if not play: continue
        audio_summary(rows)
        fps = [p['fps'] for p in play]
        print(f'  fps     median {statistics.median(fps):5.1f}  p10 {pct(fps, .1):5.1f}  p90 {pct(fps, .9):5.1f}')
        for key, label in (('ppf', 'passes/frame'), ('ms_pass', 'ms/pass'), ('pass_share', 'pass share of wall'), ('busy', 'busy ms (EMA)')):
            xs = [p[key] for p in play if p[key] == p[key] and p[key] is not None]
            if xs: print(f'  {label:18s} median {statistics.median(xs):7.3f}  p10 {pct(xs, .1):7.3f}  p90 {pct(xs, .9):7.3f}')
        tiers = [p['tier'] for p in play]
        print('  tier seconds:', {t: round(sum(1.2 for x in tiers if x == t), 1) for t in sorted(set(tiers))})
        cpus = [p['cpu'] for p in play if p['cpu'] is not None]
        if cpus: print(f'  engine thread CPU/wall  median {statistics.median(cpus):.2f}  p10 {pct(cpus, .1):.2f}  p90 {pct(cpus, .9):.2f}')
        for key, label in (('captures', 'pad captures/frame'), ('reuses', 'pad reuses/frame'),
                           ('cache_waits', 'cache waits/frame'), ('cache_wait_ms', 'cache wait ms/frame'),
                           ('reads', 'cache reads/frame'), ('read_ms', 'cache read ms/frame')):
            xs = [p[key] for p in play if p.get(key) is not None]
            if xs: print(f'  {label:19s} median {statistics.median(xs):7.3f}  p90 {pct(xs, .9):7.3f}  mean {statistics.mean(xs):7.3f}')
        if 'guestThreadQoS' in report:
            print(f'  host switches: guest threads {report.get("guestThreadQoS")}, pad reuse {report.get("controllerReuseMicroseconds")} us, '
                  f'cache waits {"block" if report.get("engineCacheWaitBlocks") else "spin"}; report writes {report.get("reportWrites")} '
                  f'(last {report.get("reportWriteMilliseconds", 0):.1f} ms, max {report.get("reportWriteMillisecondsMax", 0):.1f} ms)')
        th = [p['thermal'] for p in play if p['thermal'] is not None]
        if th: print('  thermal state seconds:', {t: round(1.2 * th.count(t), 1) for t in sorted(set(th))})
        # Layer alignment (Build76+, off unless HALO_LAYER_ALIGN=1 or the settings
        # toggle): per layer, how far its camera was from the newest centre's and
        # how far it was turned. The records carry running sums; the difference
        # between consecutive records is attributed to the later record's
        # `enabled`, so a run that toggles it gives an on/off A/B. Off, the
        # offsets are still measured and nothing is turned. These are camera
        # offsets, not the visible join error: judge that from a recording.
        align = [r['panoramaLayerAlign'] for r in rows if isinstance(r.get('panoramaLayerAlign'), dict)]
        if align:
            last = align[-1]
            print(f'  layer alignment: enabled at end={last.get("enabled")} engine frames {last.get("frames", 0)}'
                  f' (off {last.get("framesOff", 0)}) aligned by level',
                  dict(zip(('full', 'half', 'capsTwistOnly', 'none'), last.get('framesByLevel', []))),
                  f' frames with a cut {last.get("framesWithCut", 0)}')
            split = {}
            for a, b in zip(align, align[1:]):
                frames = b.get('frames', 0) - a.get('frames', 0)
                if frames <= 0: continue
                state = split.setdefault(bool(b.get('enabled')), dict(frames=0, misaligned=[0.0] * 10, applied=[0.0] * 10))
                state['frames'] += frames
                for key, name in (('misaligned', 'misalignedDegreesSum'), ('applied', 'appliedDegreesSum')):
                    now, before = b.get(name, [0] * 10), a.get(name, [0] * 10)
                    for k in range(min(10, len(now))): state[key][k] += now[k] - (before[k] if k < len(before) else 0)
            names = {0: 'L0 -60', 1: 'L1 0', 2: 'L2 +60', 5: 'L5 sky', 6: 'L6 floor', 7: 'L7 +120', 8: 'L8 180', 9: 'L9 -120'}
            for enabled, state in sorted(split.items()):
                n = state['frames']
                print(f'  alignment {"on " if enabled else "off"} {n:6d} frames, mean deg misaligned -> turned:', '  '.join(
                    f'{name} {state["misaligned"][k] / n:.1f}->{state["applied"][k] / n:.1f}' for k, name in names.items()))
            print('  run max deg misaligned -> turned:', '  '.join(
                f'{name} {last["misalignedDegreesMax"][k]:.0f}->{last["appliedDegreesMax"][k]:.0f}' for k, name in names.items()
                if k < len(last.get('misalignedDegreesMax', []))))
        if 'engineThreadFramesByCPU' in report:
            h = report['engineThreadFramesByCPU']; n = sum(h) or 1
            print(f'  cores: {report.get("performanceCores")} performance, {report.get("efficiencyCores")} efficiency')
            print('  engine frames by CPU:', ' '.join(f'{i}:{100 * v / n:.0f}%' for i, v in enumerate(h) if v))
        if 'engineProgramCompiles' in report:
            # Pipelines built at a draw on the engine thread (Build76+: plus waits
            # for a background build), and how many 1.2 s intervals they stalled.
            stall = lambda r: r.get('engineProgramCompileSeconds', 0) + r.get('engineProgramWaitSeconds', 0)
            hitches = sum(1 for a, b in zip(rows, rows[1:]) if stall(b) - stall(a) > 0.05)
            line = f'  pipelines: engine built {report["engineProgramCompiles"]} in {report["engineProgramCompileSeconds"]:.2f} s'
            if 'engineProgramWaits' in report:
                line += (f', waited {report["engineProgramWaits"]}x {report["engineProgramWaitSeconds"]:.2f} s; background {report["engineProgramBackgroundPipelines"]} pipelines'
                         f' + {report["engineProgramBackgroundFunctions"]} functions in {report["engineProgramBackgroundSeconds"]:.1f} s; prewarm hits {report["engineProgramPrewarmHits"]}'
                         f', archive hits {report["engineProgramArchiveHits"]}')
            print(line + f'; intervals with >50 ms of pipeline stall: {hitches}')
        core_summary(pairs, report)


if __name__ == "__main__":
    main(sys.argv[1:])
