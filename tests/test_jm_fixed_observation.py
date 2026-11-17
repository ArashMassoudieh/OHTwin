"""Fixed membership must never silently substitute a different probe."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1]/'deployments/JM_sensor_experiment/tools/check_observation_sensitivity.py'
spec = importlib.util.spec_from_file_location('jm_sensitivity', path)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class FixedObservationTest(unittest.TestCase):
    def row(self, values):
        return {'timestamp':'2026-11-04T12:07:30+00:00', 'per_probe':dict(zip(m.SENSORS,values)), 'used_probes':m.SENSORS[:2]}

    def test_each_fixed_channel_contributes_equally(self):
        row = m.fixed_observation(self.row([90,90,60]), 'fixed_three_mean')
        self.assertEqual(row['value'],80)
        self.assertEqual(row['used_probes'],m.SENSORS)
        self.assertEqual(row['timestamp'],'2026-11-04T12:07:30+00:00')

    def test_missing_channel_is_omitted_not_substituted(self):
        row = self.row([90,90,60])
        del row['per_probe'][m.SENSORS[2]]
        self.assertIsNone(m.fixed_observation(row,'fixed_three_mean'))
        self.assertEqual(m.fixed_observation(row,'a1_only_control')['value'],90)

    def test_a1_control_does_not_follow_other_probes(self):
        a=m.fixed_observation(self.row([100,75,60]),'a1_only_control')
        b=m.fixed_observation(self.row([80,75,70]),'a1_only_control')
        self.assertEqual(a['value'],b['value'])

if __name__ == '__main__':
    unittest.main()
