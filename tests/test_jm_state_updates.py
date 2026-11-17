"""Causal OHQ state update and correction-volume accounting."""
import csv
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
DEP=ROOT/'deployments/JM_sensor_experiment'
RUNNER=DEP/'outputs/experiment'

@unittest.skipUnless(RUNNER.exists(),'Build experiment runner')
class StateUpdateTest(unittest.TestCase):
    def test_update_waits_for_bin_end_and_future_data_cannot_change_forecast(self):
        t0=46000.
        data={'start':t0,'end':t0+2,'train_end':t0+.5,'initial_index':80.,'observation_sigma':5.,
              'rain':[[t0+i/24,t0+(i+1)/24,0.] for i in range(48)],
              'training':[[t0+.1,76.]],
              'validation':[[t0+.5+i/96,80.] for i in range(144)],
              'rolling_state_test':True,'state_update_gain':.5,'fixed_parameters':[12.,.5]}
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp)
            def run():
                (out/'input.json').write_text(json.dumps(data))
                result=subprocess.run([str(RUNNER),str(DEP/'model/JM.ohq'),str(ROOT/'resources'),str(out/'input.json'),str(out)],capture_output=True,text=True,timeout=60)
                self.assertEqual(result.returncode,0,result.stderr[-1500:])
                with (out/'rolling_forecasts.csv').open() as f:
                    return [{k:float(v) for k,v in row.items()} for row in csv.DictReader(f)]
            rows=run();first=rows[0]
            self.assertAlmostEqual(first['origin'],t0+.5+.5/96,places=7)
            self.assertGreaterEqual(first['origin'],first['observation_available_at'])
            self.assertAlmostEqual(first['updated_index'],.5*first['prior_index']+40.,places=6)
            self.assertAlmostEqual(first['state_increment_m3'],3.617477152128*(first['updated_index']-first['prior_index'])/100,places=7)
            self.assertAlmostEqual(first['prediction'],first['updated_index']*__import__('math').exp(-.5/24),delta=.15)
            first_origin=first['origin']
            early=[r for r in rows if r['origin']==first_origin]
            data['validation'][48][1]=0. # a future reading, never available at first origin
            rerun=run()
            self.assertEqual(early,[r for r in rerun if r['origin']==first_origin])

if __name__=='__main__':unittest.main()
