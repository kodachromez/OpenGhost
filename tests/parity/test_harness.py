"""Focused harness safety/measurement checks; never launches a visible platform."""
import os
from pathlib import Path
import subprocess
import unittest
import numpy as np
from fixtures import fixtures
from metrics import measure

class HarnessTests(unittest.TestCase):
    def test_zero_tolerance(self):
        a = np.zeros((2, 3, 3), dtype=np.uint8)
        self.assertEqual(measure(a, a)['changed_pixels'], 0)
        b = a.copy(); b[0, 0, 0] = 1
        m = measure(a, b)
        self.assertEqual(m['changed_pixels'], 1)
        self.assertEqual(m['over8_percent'], 0)  # diagnostic only, NOT a match
        b[1, 2, 2] = 255
        self.assertEqual(measure(a, b)['changed_pixels'], 2)
        self.assertEqual(measure(a, b)['max_channel'], 255)  # no uint8 wraparound

    def test_dimensions_refuse(self):
        with self.assertRaises(ValueError):
            measure(np.zeros((2, 3, 3)), np.zeros((3, 2, 3)))

    def test_inventory(self):
        fs = fixtures()
        self.assertEqual(len(fs), len({f['id'] for f in fs}))
        self.assertEqual(sum('diagram' in f for f in fs), 44)  # 43 families + light repeat
        for f in fs:
            self.assertLessEqual(f.get('width', 1280), 2048)
            self.assertLessEqual(f.get('height', 840), 1400)
            if f.get('manual'):
                self.assertTrue(f['id'].startswith('manual-visible-'))
                self.assertIn('reason', f)

    def test_pre_application_visible_refusal(self):
        binary = Path(__file__).resolve().parents[2] / 'build-release/openghost-native'
        for argument in ['--parity-manifest', '--parity-manifest=/nonexistent']:
            # No DISPLAY/Wayland to connect to even if the guard regresses.
            env = dict(os.environ, QT_QPA_PLATFORM='xcb', DISPLAY='', WAYLAND_DISPLAY='')
            p = subprocess.run([str(binary), argument], env=env, capture_output=True, timeout=5)
            self.assertEqual(p.returncode, 2)
            self.assertIn(b'Parity requires', p.stderr)
        env['QT_QPA_PLATFORM'] = 'offscreen'
        p = subprocess.run([str(binary), '--parity-manifest=/nonexistent', '-platform', 'xcb'],
                           env=env, capture_output=True, timeout=5)
        self.assertEqual(p.returncode, 2)
        self.assertIn(b'Parity requires', p.stderr)

if __name__ == '__main__':
    unittest.main()
