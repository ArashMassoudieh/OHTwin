#!/usr/bin/env python3
"""
AnacostiaIQ -> OpenHydroTwin sensor bridge, for JM_assimilation_3cell.

Serves `GET /outputs/selected_output.csv` and `/outputs/selected_output_meta.json`
in the exact shape DTAssimilation.cpp expects from a truth_csv_url -- the same
role JM_truth's simulated output normally plays in the Bioretention examples,
just backed by the real field station's live sensor API instead of another
simulation run. Nothing else in DTAssimilation needs to change: it already
treats "truth" as "whatever this URL returns," with no assumption it came
from a simulation.

Field station status (checked 2026-11-11 against the live dashboard at
http://54.213.147.59/SensorDashboard.html): the original field-station-01
is retired. It was physically replaced by jm-cua-twin-pi (station id
jm_cua_twin_pi_staging, "John McCormack Field Station"), which has been
live and posting since its commissioning on 2026-10-30. The dashboard's
own config.json confirms this is the live feed, not a demo/fallback
("demo_fallback": false). This bridge pulls from that replacement Pi.

Mapping (confirmed 2026-11-11 -- see deployments/JM_assimilation_3cell/config.json
for the corresponding calibration_observations entry):
  - MaxBotix "Ponding Depth" (sensor id: staging_maxbotix_depth, project
    John-McCormack) -> "Pond 1 water depth"
      Sensors are physically installed in the highest-elevation cell only
      (confirmed on site), which is Pond 1 / Engineered Soil 1 in this
      corrected 3-column model. HC-SR04 "Inflow Weir Head"
      (staging_hcsr04_depth) is NOT used here -- by name it measures at an
      inflow/weir structure, not the pond itself, and there's no matching
      model observation for that distinct quantity yet. Revisit if that
      turns out to be the wrong read.
  - Moisture probe A0 (sensor id: staging_moisture_sensor) -> "Soil 1 moisture"
      (A1/staging_moisture_a1 and A2/staging_moisture_a2 exist too, but
      only one instrumented cell maps into this 3-column model for now.)
  - The pressure transducer (planned for the deep inspection well) is NOT
    wired in yet -- staging_pressure_loop_current_raw is still uncalibrated
    current, not depth, and the model has no "inspection well" object
    distinct from the pond surface, so its correspondence needs to be
    designed first.
  - The other two cells (Pond 2/3, Soil 2/3) carry no real-sensor feed and
    keep running on the model alone -- estimating them from the one
    instrumented cell is explicitly future work, not attempted here.

CAVEATS -- read before trusting any assimilation result this produces:
  - Depth datum: field reports inches from its OWN configured zero reference
    (totalLength - measured, see MaxbotixSensor in the AnacostiaIQ repo);
    the model's "Pond 1 water depth" is meters above JM Pond 1's
    bottom_elevation. This bridge only does the unit conversion (in -> m).
    It does NOT reconcile the two zero datums, which most likely differ.
    Treat early runs as a plumbing/connectivity check, not a calibrated
    result, until that reconciliation is done deliberately.
  - Moisture scale: field reports a 0-100% value normalized against its own
    dry/wet calibration points (and the dashboard's own notes flag that
    calibration as provisional/non-quantitative for both A0 and A1); the
    model's theta is volumetric water content (0 to theta_sat, roughly
    0-0.4 here). This bridge applies a simple linear placeholder,
    theta = theta_res + (pct/100)*(theta_sat - theta_res), with no real
    site soil calibration behind it.
  - Data history: the replacement Pi's own data starts 2026-10-30T15:31:14Z
    ("_commissioningComment" in the dashboard config). Earlier bench/staging
    records exist but aren't representative of the as-installed sensor --
    this bridge's default window is short enough to avoid pulling that in,
    but widen --window-days with that in mind.

Usage:
    python3 sensor_bridge.py [--port 8198] [--api http://54.213.147.59:5000]
                             [--project John-McCormack] [--window-days 14]
"""

import argparse
import datetime
import http.server
import json
import sys
import urllib.request
from urllib.parse import quote

OHQ_EPOCH = datetime.datetime(1899, 12, 30)

# Model constants this bridge's unit/scale conversions depend on -- keep in
# sync with deployments/JM_assimilation_3cell/model/JM.ohq if that changes.
THETA_RES = 0.05
THETA_SAT = 0.4
IN_TO_M = 0.0254

FIELD_SENSORS = {
    "staging_maxbotix_depth": {
        "observation_name": "Pond 1 water depth",
        "convert": lambda v: v * IN_TO_M,  # in -> m, datum NOT reconciled
    },
    "staging_moisture_sensor": {
        "observation_name": "Soil 1 moisture",
        "convert": lambda v: THETA_RES + (v / 100.0) * (THETA_SAT - THETA_RES),
    },
}


def iso_to_ohq_serial(iso_ts: str) -> float:
    # Server emits isoformat() + "Z"; fromisoformat() before Python 3.11
    # doesn't accept the trailing Z, so strip it -- OHQ serial dates are
    # naive/UTC anyway, same as the server's own utcnow() timestamps.
    dt = datetime.datetime.fromisoformat(iso_ts.rstrip("Z"))
    return (dt - OHQ_EPOCH).total_seconds() / 86400.0


def fetch_sensor_series(api_base: str, project: str, sensor_id: str, window_days: int):
    # The API's start/end are compared lexicographically against stored ISO
    # timestamp strings (DynamoDB Key.between on a string attribute), not
    # parsed as dates. A bare date like "2026-11-11" sorts BEFORE any
    # same-day "2026-11-11T16:59:30" timestamp (shorter string, common
    # prefix), so it silently excludes the entire current day -- confirmed
    # by hand against the live API. Full isoformat() timestamps avoid that.
    end = datetime.datetime.utcnow()
    start = end - datetime.timedelta(days=window_days)
    url = (
        f"{api_base}/sensor/{quote(sensor_id)}"
        f"?project={quote(project)}&start={start.isoformat()}&end={end.isoformat()}"
    )
    try:
        with urllib.request.urlopen(url, timeout=20) as resp:
            rows = json.loads(resp.read())
    except Exception as e:
        print(f"[bridge] fetch failed for {sensor_id}: {e}", file=sys.stderr)
        return []
    series = []
    for row in rows:
        try:
            t = iso_to_ohq_serial(row["timestamp"])
            v = float(row["value"])
        except (KeyError, ValueError, TypeError):
            continue
        # -1 is MaxbotixSensor's/HealthMonitor's error sentinel (out-of-range
        # reading or read timeout), not a real measurement -- 0 IS a real
        # reading (MB7389 R5000 "no target" = genuine zero ponding depth),
        # so only negative values are dropped here.
        if v < 0:
            continue
        series.append((t, v))
    series.sort(key=lambda p: p[0])
    return series


def build_csv(series_by_name):
    header_cells = []
    for name in series_by_name:
        header_cells.extend(["t", f" {name}"])
    lines = [",".join(header_cells)]
    max_len = max((len(s) for s in series_by_name.values()), default=0)
    for i in range(max_len):
        row = []
        for series in series_by_name.values():
            if i < len(series):
                t, v = series[i]
                row.extend([f"{t:.5f}", f"{v:.6g}"])
            else:
                row.extend(["", ""])
        lines.append(",".join(row))
    return "\n".join(lines) + "\n"


def build_meta(series_by_name, window_days):
    latest = {}
    for name, series in series_by_name.items():
        latest[name] = series[-1] if series else None
    return json.dumps(
        {
            "deployment_name": "JM_assimilation_3cell",
            "source": "AnacostiaIQ field station sensor bridge (not a simulation)",
            "generated_utc": datetime.datetime.utcnow().isoformat() + "Z",
            "window_days": window_days,
            "latest_point_per_observation": {
                name: (
                    {"ohq_serial": t, "value": v} if point else None
                )
                for name, point in latest.items()
                for t, v in ([point] if point else [(None, None)])
            },
            "caveats": [
                "Depth datum (field zero vs model Pond 1 bottom_elevation) is NOT reconciled -- unit-converted only.",
                "Moisture scale (field 0-100% dry/wet calibration vs model volumetric theta) is a linear placeholder, not a real soil calibration.",
                "Source is the replacement Pi (jm_cua_twin_pi_staging), live since 2026-10-30; earlier bench/staging data is excluded by the window.",
            ],
        },
        indent=4,
    )


class BridgeHandler(http.server.BaseHTTPRequestHandler):
    api_base = None
    project = None
    window_days = 14

    def _send(self, body: str, content_type: str):
        data = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path not in ("/outputs/selected_output.csv", "/outputs/selected_output_meta.json"):
            self.send_response(404)
            self.end_headers()
            return

        series_by_name = {}
        for sensor_id, spec in FIELD_SENSORS.items():
            raw = fetch_sensor_series(self.api_base, self.project, sensor_id, self.window_days)
            series_by_name[spec["observation_name"]] = [
                (t, spec["convert"](v)) for t, v in raw
            ]

        if self.path.endswith(".csv"):
            self._send(build_csv(series_by_name), "text/csv")
        else:
            self._send(build_meta(series_by_name, self.window_days), "application/json")

    def log_message(self, fmt, *args):
        print(f"[bridge] {self.address_string()} - {fmt % args}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8198)
    ap.add_argument("--api", default="http://54.213.147.59:5000")
    ap.add_argument("--project", default="John-McCormack")
    ap.add_argument("--window-days", type=int, default=14)
    args = ap.parse_args()

    BridgeHandler.api_base = args.api
    BridgeHandler.project = args.project
    BridgeHandler.window_days = args.window_days

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), BridgeHandler)
    print(f"[bridge] serving on http://127.0.0.1:{args.port}/outputs/selected_output.csv")
    print(f"[bridge] pulling from {args.api}, window={args.window_days}d")
    print(f"[bridge] mapping: {', '.join(s['observation_name'] for s in FIELD_SENSORS.values())}")
    server.serve_forever()


if __name__ == "__main__":
    main()
