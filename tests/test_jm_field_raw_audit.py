"""Raw-signal audit must preserve measurements and distinguish step/spike evidence."""
import importlib.util
from pathlib import Path
import sys
import unittest
import datetime as dt

folder=Path(__file__).resolve().parents[1]/'deployments/JM_sensor_experiment/tools'
sys.path.insert(0,str(folder))
spec=importlib.util.spec_from_file_location('field_audit',folder/'audit_field_raw.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
sys.path.pop(0)

class FieldRawAuditTest(unittest.TestCase):
    def rows(self,values):
        start=dt.datetime(2026,11,17,18,30,tzinfo=dt.timezone.utc)
        return [{'timestamp':(start+dt.timedelta(minutes=i)).isoformat(),'value':v} for i,v in enumerate(values)]
    def test_stable_precise_signal_is_not_flagged_or_rewritten(self):
        rows=self.rows([.419125,.41925,.419125,.41925,.419125,.41925])
        original=[dict(r) for r in rows]
        result=m.summarize(rows,dt.datetime(2026,11,17,tzinfo=dt.timezone.utc))
        self.assertFalse(result['large_adjacent_changes'])
        self.assertFalse(result['causal_diagnostic_flags'])
        self.assertEqual(rows,original)
        self.assertEqual(result['new_records_beyond_two_decimals'],6)
    def test_diagnostic_flags_do_not_turn_a_sustained_step_into_a_spike(self):
        rows=self.rows([.4]*6+[.85]*12)
        result=m.summarize(rows,dt.datetime(2026,11,17,tzinfo=dt.timezone.utc))
        self.assertEqual(len(result['large_adjacent_changes']),1)
        self.assertIsNone(result['large_adjacent_changes'][0]['return_within_10_min'])
        self.assertTrue(result['causal_diagnostic_flags'])
        prefix=m.summarize(rows[:8],dt.datetime(2026,11,17,tzinfo=dt.timezone.utc))
        self.assertEqual(prefix['causal_diagnostic_flags'],result['causal_diagnostic_flags'][:len(prefix['causal_diagnostic_flags'])])

if __name__=='__main__':unittest.main()
