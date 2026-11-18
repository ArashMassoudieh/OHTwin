#!/usr/bin/env python3
"""Live JM sensor observations. Shallow moisture is a provisional dry/wet index,
not measured volumetric water content. A fixed A0/A2 pair provides one Cell 1
observation; A1 remains available separately because its voltage jumps are unresolved.
HC flow and pressure current are diagnostic, not fitted while physical issues remain.
"""

import argparse
import datetime
import http.server
import json
import math
import statistics
from concurrent.futures import ThreadPoolExecutor
import sys
import urllib.request
from urllib.parse import quote, urlsplit

OHQ_EPOCH = datetime.datetime(1899, 12, 30)

# Model constants this bridge's unit/scale conversions depend on -- keep in
# sync with deployments/JM_assimilation_3cell/model/JM.ohq if that changes.
THETA_RES = 0.05
THETA_SAT = 0.4
IN_TO_M = 0.0254

FIELD_SENSORS = {
    "staging_maxbotix_depth": {"observation_name": "Pond 1 water depth", "convert": lambda v: v * IN_TO_M},
    "staging_moisture_sensor_raw": {"observation_name": "Cell 1 A0 voltage", "convert": float},
    "staging_moisture_a1_raw": {"observation_name": "Cell 1 A1 voltage", "convert": float},
    "staging_moisture_a2_raw": {"observation_name": "Cell 1 A2 voltage", "convert": float},
    "staging_inflow_flow_rate": {"observation_name": "Cell 1 inlet flow diagnostic", "convert": lambda v: v * 86.4},
    "staging_inflow_weir_head": {"observation_name": "Inlet weir head diagnostic", "convert": lambda v: v * IN_TO_M},
    "staging_pressure_loop_current_raw": {"observation_name": "Pressure current diagnostic", "convert": float},
}
CALIBRATION = {"A0": (17155, 5600), "A1": (17155, 2400), "A2": (17155, 6400)}

def shallow_observations(series, now_serial):
    """Completed UTC 15-minute medians; no gap filling or changing membership."""
    channels = {}
    for channel in CALIBRATION:
        grouped = {}
        for t, voltage in series.get(f"Cell 1 {channel} voltage", []):
            if not math.isfinite(voltage) or not 0 < voltage < 4.096:
                continue
            bucket = math.floor(t * 96 + 1e-7)
            if (bucket + 1) / 96 > now_serial:
                continue
            grouped.setdefault(bucket, []).append(voltage)
        dry, wet = CALIBRATION[channel]
        channels[channel] = {
            bucket: min(100., max(0., 100 * (dry - statistics.median(values) / .000125) / (dry - wet)))
            for bucket, values in grouped.items() if len(values) >= 5
        }
    result = {
        f"Cell 1 {channel} moisture index": [((b + 1) / 96, v) for b, v in sorted(values.items())]
        for channel, values in channels.items()
    }
    # Timestamp at bin END: never imply that the median was available earlier.
    result["Cell 1 shallow moisture index"] = [
        ((b + 1) / 96, (channels["A0"][b] + channels["A2"][b]) / 2)
        for b in sorted(channels["A0"].keys() & channels["A2"].keys())
    ]
    return result


def iso_to_ohq_serial(iso_ts: str) -> float:
    # Server emits isoformat() + "Z"; fromisoformat() before Python 3.11
    # doesn't accept the trailing Z, so strip it -- OHQ serial dates are
    # naive/UTC anyway, same as the server's own utcnow() timestamps.
    dt = datetime.datetime.fromisoformat(iso_ts.replace("Z", "+00:00"))
    if dt.tzinfo is not None:
        dt = dt.astimezone(datetime.timezone.utc).replace(tzinfo=None)
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
        if not math.isfinite(v) or v < 0:
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
            "observation_mapping": {
                "Pond 1 water depth": {"unit": "m", "status": "provisional field zero; not independently surveyed"},
                "Cell 1 shallow moisture index": {"unit": "% index", "probes": ["A0", "A2"], "layer": "upper 0.3048 m (model discretization, not surveyed probe depth)", "status": "relative dry/wet proxy; not calibrated VWC"},
                "Cell 1 inlet flow diagnostic": {"unit": "m3/day", "status": "not assimilated: leaves in box; reconstructed history includes restored weir calibration"},
                "Pressure current diagnostic": {"unit": "mA", "status": "not assimilated: no field pressure-depth calibration"},
            },
            "caveats": [
                "MB field zero was adopted from a dry reference; physical well/pond correspondence remains provisional.",
                "Shallow index uses fixed raw-voltage calibration and A0/A2 membership; missing either probe leaves a gap. A1 remains diagnostic due unresolved jumps.",
                "Moisture is a relative index, not calibrated VWC. The model index is a provisional effective-saturation proxy in its upper one-foot layer.",
                "All moisture probes belong to Cell 1; Cells 2/3 have no direct observations.",
                "HC is diagnostic only; no measured flow is added to modeled runoff, avoiding double counting.",
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
        # The viewer appends ?_t=... to bypass caches; route by path only.
        path = urlsplit(self.path).path
        if path not in ("/outputs/selected_output.csv", "/outputs/selected_output_meta.json"):
            self.send_response(404)
            self.end_headers()
            return

        def fetch(item):
            sensor_id, spec = item
            raw = fetch_sensor_series(self.api_base, self.project, sensor_id, self.window_days)
            return spec["observation_name"], [(t, spec["convert"](v)) for t, v in raw]
        with ThreadPoolExecutor(max_workers=7) as pool:
            series_by_name = dict(pool.map(fetch, FIELD_SENSORS.items()))
        now_serial = (datetime.datetime.utcnow() - OHQ_EPOCH).total_seconds() / 86400
        series_by_name.update(shallow_observations(series_by_name, now_serial))

        if path.endswith(".csv"):
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
