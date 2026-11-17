import datetime as dt
import importlib.util
from pathlib import Path
import unittest

path=Path(__file__).resolve().parents[1]/'deployments/JM_sensor_experiment/tools/prepare_data.py'
spec=importlib.util.spec_from_file_location('jm_prepare',path)
m=importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class MoistureObservationTest(unittest.TestCase):
    def rows(self, value, time='2026-11-01T12:01:00Z'):
        return [{'timestamp':time,'value':value}]

    def combine(self, data):
        return m.combine(data,dt.datetime(2026,11,1,12,15,tzinfo=dt.timezone.utc))

    def test_one_disagreeing_probe_does_not_dominate(self):
        good,bad=self.combine({'a':self.rows(88),'b':self.rows(90),'c':self.rows(0)})
        self.assertEqual(len(good),1)
        self.assertEqual(good[0]['value'],89)
        self.assertEqual(good[0]['excluded_probes'],['c'])
        self.assertGreaterEqual(good[0]['sigma'],5)

    def test_two_disagreeing_probes_are_not_an_observation(self):
        good,bad=self.combine({'a':self.rows(90),'b':self.rows(10)})
        self.assertFalse(good)
        self.assertEqual(len(bad),1)

    def test_missing_invalid_and_nonfinite_values_are_not_forward_filled(self):
        for bad in [-1,101,float('nan'),float('inf')]:
            good,_=self.combine({'a':self.rows(90),'b':self.rows(bad)})
            self.assertFalse(good)
        good,_=self.combine({'a':self.rows(90),'b':self.rows(90,'2026-11-01T12:16:00Z')})
        self.assertFalse(good)

    def test_each_probe_has_one_vote_despite_different_sampling_rates(self):
        good,_=self.combine({'a':self.rows(80)*100,'b':self.rows(90),'c':self.rows(100)})
        self.assertEqual(good[0]['value'],90)

if __name__=='__main__':
    unittest.main()
