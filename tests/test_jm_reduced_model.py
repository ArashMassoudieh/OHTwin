"""Integration checks against the built OHQ experiment runner (no network)."""
import csv
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO=Path(__file__).resolve().parents[1]
DEP=REPO/'deployments/JM_sensor_experiment'
RUNNER=DEP/'outputs/experiment'

@unittest.skipUnless(RUNNER.exists(),'Build with deployments/JM_sensor_experiment/tools/build_runner.py')
class ReducedModelTest(unittest.TestCase):
    def test_dry_recession_and_strict_holdout_split(self):
        t0=46000.
        training=[[t0+i/96,80*math.exp(-.5*i/96)] for i in range(1,192)]
        data={'start':t0,'end':t0+3,'train_end':t0+2,'initial_index':80.,'observation_sigma':5.,
              'rain':[[t0+i/24,t0+(i+1)/24,0.] for i in range(72)],
              'training':list(training),'validation':[[t0+2.5,80*math.exp(-1.25)]]}
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp);input_path=out/'input.json';input_path.write_text(json.dumps(data))
            args=[str(RUNNER),str(DEP/'model/JM.ohq'),str(REPO/'resources'),str(input_path),str(out)]
            result=subprocess.run(args,capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr[-1500:])
            with (out/'baseline.csv').open() as f:
                reader=csv.reader(f);next(reader);rows=list(reader)
            for row in rows:
                if not row[0] or not row[1]:continue
                t,v=float(row[0]),float(row[1])
                self.assertAlmostEqual(v,80*math.exp(-.5*(t-t0)),delta=.2)
                for i in [3,7,11]:self.assertAlmostEqual(float(row[i]),0.,delta=1e-8)
            params=json.loads((out/'parameters.json').read_text())
            self.assertFalse(params['holdout_used_for_fitting'])
            self.assertAlmostEqual(params['drainage_rate_per_day'],.5,delta=.08)
            data['training'].append([t0+2.5,50.])
            input_path.write_text(json.dumps(data))
            result=subprocess.run(args,capture_output=True,text=True,timeout=90)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('holdout leakage',result.stderr)
            data['training']=training
            data['rain'][24][2]=.003
            input_path.write_text(json.dumps(data))
            result=subprocess.run(args,capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr[-1500:])
            with (out/'baseline.csv').open() as f:
                reader=csv.reader(f);next(reader)
                wet_rows=[[float(x) if x else float('nan') for x in row] for row in reader]
            peaks=[max(row[i] for row in wet_rows) for i in [3,7,11]]
            self.assertGreater(peaks[0],peaks[1])
            self.assertGreater(peaks[1],peaks[2])
            self.assertGreater(peaks[2],0)

if __name__=='__main__':unittest.main()
