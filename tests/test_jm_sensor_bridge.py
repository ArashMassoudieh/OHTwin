"""Regression checks for viewer cache-busting URLs against the real HTTP handler."""
import importlib.util
import json
from pathlib import Path
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import urlopen

SCRIPT = Path(__file__).resolve().parents[1] / 'deployments/JM_assimilation_3cell/bridge/sensor_bridge.py'
spec = importlib.util.spec_from_file_location('jm_sensor_bridge', SCRIPT)
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class BridgeRoutingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mock_fetch = patch.object(bridge, 'fetch_sensor_series', return_value=[(46340., 20.)])
        cls.mock_fetch.start()
        cls.server = bridge.http.server.ThreadingHTTPServer(('127.0.0.1', 0), bridge.BridgeHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f'http://127.0.0.1:{cls.server.server_port}'

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.thread.join()
        cls.server.server_close()
        cls.mock_fetch.stop()

    def test_csv_with_cache_buster_matches_plain_url(self):
        with urlopen(self.base + '/outputs/selected_output.csv') as response:
            plain = response.read()
        with urlopen(self.base + '/outputs/selected_output.csv?_t=123') as response:
            self.assertEqual(response.headers.get_content_type(), 'text/csv')
            self.assertEqual(response.read(), plain)
        self.assertIn(b'Pond 1 water depth', plain)
        self.assertIn(b'Cell 1 shallow moisture index', plain)

    def test_metadata_with_cache_buster_is_json(self):
        with urlopen(self.base + '/outputs/selected_output_meta.json?_t=123') as response:
            self.assertEqual(response.headers.get_content_type(), 'application/json')
            self.assertEqual(json.load(response)['deployment_name'], 'JM_assimilation_3cell')

    def test_unknown_route_remains_not_found(self):
        with self.assertRaises(HTTPError) as caught:
            urlopen(self.base + '/missing?_t=123')
        self.assertEqual(caught.exception.code, 404)


class ShallowObservationTest(unittest.TestCase):
    def series(self, a0=.92, a2=1.32):
        t=46000.
        return {"Cell 1 A0 voltage": [(t+i/86400,a0) for i in range(1,6)],
                "Cell 1 A2 voltage": [(t+i/86400,a2) for i in range(1,6)]}

    def test_fixed_pair_uses_both_probes_and_bin_end(self):
        r=bridge.shallow_observations(self.series(),46001.)
        pair=r['Cell 1 shallow moisture index']
        self.assertEqual(len(pair),1)
        self.assertAlmostEqual(pair[0][0],46000.+1/96)
        expected=((17155-.92/.000125)/(17155-5600)+(17155-1.32/.000125)/(17155-6400))*50
        self.assertAlmostEqual(pair[0][1],expected)

    def test_missing_probe_does_not_change_membership(self):
        d=self.series();d.pop('Cell 1 A2 voltage')
        self.assertEqual(bridge.shallow_observations(d,46001.)['Cell 1 shallow moisture index'],[])

    def test_future_incomplete_and_invalid_samples_withheld(self):
        self.assertEqual(bridge.shallow_observations(self.series(),46000.+1/192)['Cell 1 shallow moisture index'],[])
        d=self.series();d['Cell 1 A2 voltage'][-1]=(46000.+5/86400,float('nan'))
        self.assertEqual(bridge.shallow_observations(d,46001.)['Cell 1 shallow moisture index'],[])

    def test_a1_diagnostic_cannot_move_pair(self):
        d=self.series();before=bridge.shallow_observations(d,46001.)
        d['Cell 1 A1 voltage']=[(46000.+i/86400,.4) for i in range(1,6)]
        after=bridge.shallow_observations(d,46001.)
        self.assertEqual(before['Cell 1 shallow moisture index'],after['Cell 1 shallow moisture index'])
        self.assertEqual(len(after['Cell 1 A1 moisture index']),1)

    def test_timezone_offsets_are_normalized(self):
        self.assertEqual(bridge.iso_to_ohq_serial('2026-11-18T09:00:00-05:00'),bridge.iso_to_ohq_serial('2026-11-18T14:00:00Z'))


if __name__ == '__main__':
    unittest.main()
