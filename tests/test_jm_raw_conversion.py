"""Guard raw ADC voltage units and fixed calibration interpretation."""
import importlib.util
from pathlib import Path
import sys
import unittest

folder=Path(__file__).resolve().parents[1]/'deployments/JM_sensor_experiment/tools'
sys.path.insert(0,str(folder))
spec=importlib.util.spec_from_file_location('jm_raw_replay',folder/'prepare_raw_replay.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
sys.path.pop(0)

class RawConversionTest(unittest.TestCase):
    def test_adc_endpoints_are_converted_using_4_096_v_full_scale(self):
        self.assertAlmostEqual(m.voltage_to_index(17155*4.096/32768,17155,5600),0)
        self.assertAlmostEqual(m.voltage_to_index(5600*4.096/32768,17155,5600),100)
        self.assertAlmostEqual(m.voltage_to_index(((17155+5600)/2)*4.096/32768,17155,5600),50)

    def test_bad_calibration_and_nonfinite_voltage_are_rejected(self):
        with self.assertRaises(ValueError):m.voltage_to_index(.8,5600,17155)
        with self.assertRaises(ValueError):m.voltage_to_index(float('nan'),17155,5600)

if __name__=='__main__':unittest.main()
