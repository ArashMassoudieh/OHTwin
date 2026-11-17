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
        self.assertIn(b'Soil 1 moisture', plain)

    def test_metadata_with_cache_buster_is_json(self):
        with urlopen(self.base + '/outputs/selected_output_meta.json?_t=123') as response:
            self.assertEqual(response.headers.get_content_type(), 'application/json')
            self.assertEqual(json.load(response)['deployment_name'], 'JM_assimilation_3cell')

    def test_unknown_route_remains_not_found(self):
        with self.assertRaises(HTTPError) as caught:
            urlopen(self.base + '/missing?_t=123')
        self.assertEqual(caught.exception.code, 404)


if __name__ == '__main__':
    unittest.main()
