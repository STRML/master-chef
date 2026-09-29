#!/usr/bin/env python3
"""The watcher must reject stale-build evidence and unsafe/device-foreign paths."""
import unittest
from watch_engine_vision_telemetry import live_candidates, accepted, summary

class WatchTests(unittest.TestCase):
    def test_candidates(self):
        run = '11111111-2222-3333-4444-555555555555'
        rows = [{'relativePath': p, 'metadata': {'lastModDate': '2026'}} for p in
                [run + '/live.json', run + '/report.json', '../live.json', '/tmp/live.json', 'not-a-uuid/live.json']]
        self.assertEqual(live_candidates({'result': {'files': rows}}), [run + '/live.json'])
    def test_build_identity(self):
        self.assertFalse(accepted({'build': '80', 'buildID': 'old', 'runID': 'a'}, 'new'))
        self.assertFalse(accepted({'build': '79', 'buildID': 'new', 'runID': 'a'}, 'new'))
        self.assertTrue(accepted({'build': '80', 'buildID': 'new', 'runID': 'a'}, 'new'))
    def test_next_build_keeps_both_identity_checks(self):
        self.assertTrue(accepted({'build': '81', 'buildID': 'new', 'runID': 'a'}, 'new', '81'))
        self.assertFalse(accepted({'build': '80', 'buildID': 'new', 'runID': 'a'}, 'new', '81'))
        self.assertFalse(accepted({'build': '81', 'buildID': 'old', 'runID': 'a'}, 'new', '81'))
        self.assertFalse(accepted({'build': '81', 'buildID': 'new'}, 'new', '81'))
    def test_summary_missing_and_partial(self):
        self.assertEqual(summary({'runID': 'a'})['maxObservedLayerAgeMs'], -1)
        self.assertEqual(summary({'runID': 'a', 'layerObservedAgeMilliseconds': [-1, 400, 0]})['maxObservedLayerAgeMs'], 400)

if __name__ == '__main__':
    unittest.main()
