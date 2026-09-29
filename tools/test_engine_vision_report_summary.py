#!/usr/bin/env python3
"""Check summary arithmetic against cumulative device telemetry and old reports."""
import contextlib
import copy
import io
import json
import math
import pathlib
import subprocess
import sys
import tempfile
import unittest

import engine_vision_report_summary as summary

TOOL = pathlib.Path(__file__).with_name('engine_vision_report_summary.py')


def record(n):
    """12 frames/s, 2 ticks/known frame, 48 ms outside passes vs 45 residual.

    Residual bucket means obey 5 + 20*ticks ms; that is a test association,
    not evidence that tick execution alone takes 20 ms on a device.
    """
    counts = [0, 2, 8, 2, 0]
    other = [count * (0.005 + 0.020 * ticks) * n for ticks, count in enumerate(counts)]
    wall = 0.936 * n
    return {
        'elapsedSeconds': float(n), 'intervalSeconds': 1.0, 'mode': 'panorama', 'menuActive': False,
        'frameSequence': 12 * n, 'enginePasses': 24 * n, 'enginePassSeconds': 0.36 * n,
        'engineThreadCPUSeconds': 0.5 * n, 'panoramaBudgetTier': 3, 'thermalState': 0, 'build': '76',
        'engineFrameSplit': {
            'snapshotAvailable': True, 'countsAvailable': True, 'countsFrames': 12 * n,
            'frames': 12 * n, 'wallSeconds': wall, 'passSeconds': 0.36 * n,
            'outsidePassSeconds': 0.576 * n, 'idleSeconds': 0.024 * n, 'waitSeconds': 0.012 * n,
            'otherSeconds': sum(other), 'cpuSeconds': wall * 0.9, 'unaccountedOffCoreSeconds': wall * 0.1,
            'ticks': 24 * n, 'tickUnknownFrames': 0, 'tickFrames': [count * n for count in counts],
            'tickOtherSeconds': other, 'tickResyncs': 0, 'tickMismatchFrames': 0, 'maxTicksPerFrame': 3,
            'stallFrames': 0, 'stallSeconds': 0.0},
        'processCores': {
            'available': True, 'cpuSeconds': 1.2 * n, 'performanceSeconds': 0.9 * n,
            'cycles': 3.48e9 * n, 'performanceCycles': 2.88e9 * n, 'instructions': 5.22e9 * n,
            'runnableSeconds': 1.5 * n, 'clusterSwitchesAvailable': True, 'clusterSwitches': 40 * n,
            'qosSeconds': {'userInteractive': 0.9 * n, 'legacy': 0.3 * n}},
        'engineThreadScheduling': {
            'available': True, 'infoResult': 0, 'policy': 1, 'priority': 47,
            'basePriority': 47, 'cpuUsagePercent': 88.0}}


def core_output(rows, report=None):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        summary.core_summary(list(zip(rows[1:], rows)), rows[-1] if report is None else report)
    return out.getvalue()


def cli_output(rows, report=None):
    with tempfile.TemporaryDirectory() as temp:
        run = pathlib.Path(temp)
        (run / 'timeline-000000.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))
        (run / 'report.json').write_text(json.dumps(rows[-1] if report is None else report))
        return subprocess.run([sys.executable, str(TOOL), str(run)], check=True, capture_output=True, text=True).stdout


class SummaryValidation(unittest.TestCase):
    def setUp(self):
        self.rows = [record(n) for n in (1, 2, 3)]

    def assert_metric(self, output, label, value):
        self.assertRegex(output, rf'{label}\s+median\s+{value}(?:\s|$)')

    def test_measured_split_clock_and_bucket_association(self):
        out = cli_output(self.rows)
        self.assert_metric(out, 'outside passes ms/fr', '48.00')
        self.assert_metric(out, 'residual ms/frame', '45.00')
        self.assert_metric(out, 'ticks/known frame', '2.00')
        self.assert_metric(out, 'engine CPU/wall', '0.90')
        self.assert_metric(out, 'proc queued cores', '0.30')
        self.assert_metric(out, 'proc P share', '0.75')
        self.assert_metric(out, 'proc effective GHz', '2.90')
        self.assert_metric(out, 'proc P effective GHz', '3.20')
        self.assert_metric(out, 'proc IPC', '1.50')
        self.assert_metric(out, 'P/E switches/s', '40.0')
        self.assertIn('residual = 5.0 ms + 20.0 ms per tick', out)
        self.assertIn('association, not isolated tick cost', out)
        self.assertIn('whole-process max(runnable time - CPU time, 0)', out)
        self.assertIn('userInteractive 75% legacy 25%', out)
        self.assertIn('engine thread now: policy 1 priority 47 (base 47)', out)
        # Existing lines keep their arithmetic: 1.2 s per play interval.
        self.assertIn("tier seconds: {3: 2.4}", out)
        self.assertIn("thermal state seconds: {0: 2.4}", out)

    def test_build75_compatibility(self):
        for row in self.rows:
            for key in ('engineFrameSplit', 'processCores', 'engineThreadScheduling'):
                del row[key]
        out = cli_output(self.rows, {})
        self.assertIn('fps     median  12.0', out)
        self.assertIn('engine thread CPU/wall  median 0.50', out)
        self.assertNotIn('core telemetry', out)

    def test_build78_audio_alignment_and_host_fields_preserved(self):
        for n, row in enumerate(self.rows, 1):
            row.update(audioFrames=48000 * n, audioNonzeroSamples=100, audioVoicePlays=2 * n,
                       soundSources=5, soundChannelsBusy=2, soundVoicesFree=0, soundVoicesHeld=2,
                       soundCacheSounds=7, soundReadsQueued=[0, 0, 3], cacheFileReads=[0, 0, 4 * n, 0],
                       controllerCaptures=12 * n, controllerReadingsReused=24 * n,
                       engineCacheWaits=12 * n, engineCacheWaitSeconds=0.06 * n,
                       cacheReaderReads=24 * n, cacheReaderReadSeconds=0.024 * n,
                       engineProgramCompiles=5 * n, engineProgramCompileSeconds=0.1 * n,
                       guestThreadQoS='utility', controllerReuseMicroseconds=1000,
                       engineCacheWaitBlocks=True, reportWrites=n, reportWriteMilliseconds=2.0,
                       reportWriteMillisecondsMax=4.0,
                       panoramaLayerAlign=dict(enabled=False, frames=12 * n, framesOff=12 * n,
                                               framesByLevel=[0, 0, 0, 12 * n], framesWithCut=0,
                                               misalignedDegreesSum=[12.0 * n] * 10,
                                               appliedDegreesSum=[0.0] * 10,
                                               misalignedDegreesMax=[1.0] * 10,
                                               appliedDegreesMax=[0.0] * 10))
        out = cli_output(self.rows)
        self.assertIn('silent in play   2.0 s in 1 stretches', out)
        self.assertIn('voice starts/s in play median 2.00; in silence 2.00', out)
        self.assertIn('sound reads queued 3-3', out)
        self.assertIn('sounds.map reads/s in play median 4.00', out)
        self.assert_metric(out, 'pad captures/frame', '1.000')
        self.assert_metric(out, 'pad reuses/frame', '2.000')
        self.assert_metric(out, 'cache wait ms/frame', '5.000')
        self.assert_metric(out, 'cache read ms/frame', '2.000')
        self.assertIn('host switches: guest threads utility, pad reuse 1000 us, cache waits block', out)
        self.assertIn('layer alignment: enabled at end=False engine frames 36', out)
        self.assertIn('alignment off     24 frames', out)
        self.assertIn('pipelines: engine built 15 in 0.30 s; intervals with >50 ms of pipeline stall: 2', out)
        self.assert_metric(out, 'proc effective GHz', '2.90')

    def test_process_counters_survive_missing_frame_snapshot(self):
        for row in self.rows:
            row['engineFrameSplit']['snapshotAvailable'] = False
        out = core_output(self.rows)
        self.assert_metric(out, 'proc effective GHz', '2.90')
        self.assert_metric(out, 'proc P share', '0.75')
        self.assertNotIn('outside passes ms/fr', out)

    def test_old_counters_without_availability_flags(self):
        for row in self.rows:
            for group in ('engineFrameSplit', 'processCores', 'engineThreadScheduling'):
                for key in list(row[group]):
                    if key.endswith('Available') or key == 'available':
                        del row[group][key]
            for key in ('outsidePassSeconds', 'countsFrames', 'tickUnknownFrames'):
                del row['engineFrameSplit'][key]
        out = core_output(self.rows)
        self.assert_metric(out, 'outside passes ms/fr', '48.00')
        self.assert_metric(out, 'proc effective GHz', '2.90')

    def test_missing_optional_counters_are_unavailable(self):
        for row in self.rows:
            for key in ('waitSeconds', 'idleSeconds', 'otherSeconds', 'ticks', 'unaccountedOffCoreSeconds'):
                del row['engineFrameSplit'][key]
            for key in ('cycles', 'instructions', 'performanceCycles', 'runnableSeconds', 'clusterSwitches'):
                del row['processCores'][key]
        out = core_output(self.rows)
        self.assert_metric(out, 'outside passes ms/fr', '48.00')
        self.assert_metric(out, 'proc P share', '0.75')
        for label in ('guest waits ms/fr', 'limiter idle ms/fr', 'residual ms/frame', 'ticks/known frame',
                      'off-core unacc ms', 'proc effective GHz', 'proc IPC', 'proc queued cores', 'P/E switches/s'):
            self.assertNotIn(label, out)

    def test_failed_process_and_scheduling_samples(self):
        for row in self.rows:
            row['processCores']['available'] = False
            row['engineThreadScheduling']['available'] = False
        out = core_output(self.rows)
        self.assert_metric(out, 'outside passes ms/fr', '48.00')
        for label in ('proc effective GHz', 'proc P share', 'P/E switches/s', 'process CPU by applied QoS', 'engine thread now'):
            self.assertNotIn(label, out)

    def test_failed_previous_sample_is_not_a_zero_baseline(self):
        self.rows[0]['processCores']['available'] = False
        out = core_output(self.rows[:2])
        self.assertNotIn('proc effective GHz', out)
        self.assertNotIn('proc P share', out)

    def test_failed_frame_snapshot(self):
        for row in self.rows:
            row['engineFrameSplit']['snapshotAvailable'] = False
        out = core_output(self.rows)
        for label in ('frame ms', 'ticks/known frame', 'residual =', 'whole run:'):
            self.assertNotIn(label, out)

    def test_cpu_sample_coverage_and_switches_availability(self):
        for row in self.rows:
            row['engineFrameSplit']['countsFrames'] -= int(row['elapsedSeconds'])
            row['processCores']['clusterSwitchesAvailable'] = False
        out = core_output(self.rows)
        self.assert_metric(out, 'outside passes ms/fr', '48.00')
        self.assertNotIn('engine CPU/wall', out)
        self.assertNotIn('off-core unacc ms', out)
        self.assertNotIn('P/E switches/s', out)
        for row in self.rows:
            row['engineFrameSplit']['countsFrames'] = row['engineFrameSplit']['frames']
            row['engineFrameSplit']['countsAvailable'] = False
        self.assertNotIn('engine CPU/wall', core_output(self.rows))

    def test_unknown_ticks_are_excluded_from_denominator(self):
        for row in self.rows:
            n = int(row['elapsedSeconds'])
            row['engineFrameSplit']['frames'] += 2 * n
            row['engineFrameSplit']['tickUnknownFrames'] = 2 * n
        out = core_output(self.rows)
        self.assert_metric(out, 'ticks/known frame', '2.00')
        self.assertIn('frames with unknown ticks 6', out)
        for row in self.rows:
            split = row['engineFrameSplit']
            split['tickFrames'] = [0] * 5
            split['tickOtherSeconds'] = [0.0] * 5
            split['tickUnknownFrames'] = split['frames']
            split['ticks'] = 0
        out = core_output(self.rows)
        self.assertNotIn('ticks/known frame', out)
        self.assertNotIn('residual =', out)

    def test_counter_reset_missing_and_bad_numbers(self):
        for key, value in (('cycles', 0), ('instructions', None), ('performanceCycles', math.nan)):
            self.rows[1]['processCores'][key] = value
        out = core_output(self.rows[:2])
        for label in ('proc effective GHz', 'proc IPC', 'proc P effective GHz'):
            self.assertNotIn(label, out)
        self.assertIsNone(summary.delta({'x': {'v': [2]}}, {'x': {'v': [1, 2]}}, 'x', 'v'))
        self.assertIsNone(summary.delta({'x': {'v': True}}, {'x': {'v': 0}}, 'x', 'v'))
        self.assertIsNone(summary.delta({'x': {'v': '2'}}, {'x': {'v': 1}}, 'x', 'v'))
        self.assertIsNone(summary.delta({'x': {'v': [1, 0]}}, {'x': {'v': [0, 1]}}, 'x', 'v'))

    def test_zero_denominators_and_queue_rounding(self):
        for row in self.rows:
            process = row['processCores']
            process['cpuSeconds'] = process['performanceSeconds'] = 0.0
            process['cycles'] = process['performanceCycles'] = 0
            process['qosSeconds'] = {'legacy': 0.0}
        out = core_output(self.rows)
        for label in ('proc effective GHz', 'proc P effective GHz', 'proc P share', 'proc IPC', 'process CPU by applied QoS'):
            self.assertNotIn(label, out)
        self.rows = [record(1), record(2)]
        self.rows[1]['processCores']['runnableSeconds'] = 2.69  # 1.19 runnable vs 1.2 CPU, inconsistent sample skew.
        self.assert_metric(core_output(self.rows), 'proc queued cores', '0.00')

    def test_successful_rusage_with_unsupported_hardware_counters(self):
        for row in self.rows:
            process = row['processCores']
            for key in ('cycles', 'performanceCycles', 'instructions'):
                process[key] = 0
        out = core_output(self.rows)
        self.assert_metric(out, 'proc P share', '0.75')
        for label in ('proc effective GHz', 'proc P effective GHz', 'proc IPC'):
            self.assertNotIn(label, out)
        # Instructions can be unavailable even when cycle counts are present.
        for row in self.rows:
            row['processCores']['cycles'] = 3.48e9 * row['elapsedSeconds']
        out = core_output(self.rows)
        self.assert_metric(out, 'proc effective GHz', '2.90')
        self.assertNotIn('proc IPC', out)

    def test_four_plus_bucket_and_degenerate_fit(self):
        fit = summary.tick_fit([5, 0, 0, 0, 5], [0.025, 0, 0, 0, 0.625], 30)
        self.assertIsNotNone(fit)
        self.assertAlmostEqual(fit[0], 5)
        self.assertAlmostEqual(fit[1], 20)
        self.assertEqual(fit[2][-1][0], 6)
        self.assertIsNone(summary.tick_fit([0, 10, 0, 0, 0], [0, 0.25, 0, 0, 0], 10))
        self.assertIsNone(summary.tick_fit([0, 0, 0, 0, 5], [0, 0, 0, 0, 0.2], 10))
        self.assertIsNone(summary.tick_fit([0, 10, 0, 0, 0], [0, 0.25, 0, 0, 0], 11))
        self.assertIsNone(summary.tick_fit([1], [0.1], 1))

    def test_legacy_cpu_counter_missing_baseline(self):
        rows = copy.deepcopy(self.rows[:2])
        for row in rows:
            for key in ('engineFrameSplit', 'processCores', 'engineThreadScheduling'):
                del row[key]
        del rows[0]['engineThreadCPUSeconds']
        self.assertNotIn('engine thread CPU/wall', cli_output(rows, {}))


if __name__ == '__main__':
    unittest.main(verbosity=2)
