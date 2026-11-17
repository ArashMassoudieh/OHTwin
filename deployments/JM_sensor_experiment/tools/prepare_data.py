#!/usr/bin/env python3
"""Build one quality-controlled Cell 1 observation from recorded A0/A1/A2 data."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime as dt
import json
import math
from pathlib import Path
import statistics
import urllib.parse
import urllib.request

SENSORS = ['staging_moisture_sensor', 'staging_moisture_a1', 'staging_moisture_a2']
EPOCH = dt.datetime(1899, 12, 30, tzinfo=dt.timezone.utc)
BIN_SECONDS = 900


def serial(t):
    return (t - EPOCH).total_seconds() / 86400


def timestamp(s):
    t = dt.datetime.fromisoformat(s.replace('Z', '+00:00'))
    return t.replace(tzinfo=dt.timezone.utc) if t.tzinfo is None else t.astimezone(dt.timezone.utc)


def combine(rows_by_sensor, end, max_deviation=15, max_spread=20):
    """No forward filling: incomplete/current bins and invalid values are not observations."""
    bins = {}
    for sensor, rows in rows_by_sensor.items():
        for row in rows:
            try:
                t, v = timestamp(row['timestamp']), float(row['value'])
            except (ValueError, KeyError, TypeError):
                continue
            if not math.isfinite(v) or not 0 <= v <= 100:
                continue
            k = int(t.timestamp()) // BIN_SECONDS
            if dt.datetime.fromtimestamp((k + 1) * BIN_SECONDS, dt.timezone.utc) > end:
                continue
            bins.setdefault(k, {}).setdefault(sensor, []).append(v)
    accepted, rejected = [], []
    for k, channels in sorted(bins.items()):
        per_probe = {s: statistics.median(v) for s, v in channels.items()}
        values = list(per_probe.values())
        center = statistics.median(values)
        consensus = {s: v for s, v in per_probe.items() if abs(v - center) <= max_deviation}
        good = list(consensus.values())
        record = {'timestamp': dt.datetime.fromtimestamp((k + .5) * BIN_SECONDS, dt.timezone.utc).isoformat(),
                  'per_probe': per_probe, 'used_probes': list(consensus),
                  'excluded_probes': [s for s in per_probe if s not in consensus]}
        if len(good) < 2 or max(good) - min(good) > max_spread:
            record['reason'] = 'fewer than two mutually consistent probes'
            rejected.append(record)
            continue
        value = statistics.median(good)
        sigma = max(5., 1.4826 * statistics.median(abs(v - value) for v in good))
        record.update(value=value, sigma=sigma, ohq_serial=serial(timestamp(record['timestamp'])))
        accepted.append(record)
    return accepted, rejected


def get_json(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return json.load(response)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--api', default='http://54.213.147.59:5000')
    ap.add_argument('--project', default='John-McCormack')
    ap.add_argument('--days', type=int, default=14)
    ap.add_argument('--holdout-days', type=float, default=3)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    end = dt.datetime.now(dt.timezone.utc)
    end = dt.datetime.fromtimestamp(int(end.timestamp()) // BIN_SECONDS * BIN_SECONDS, dt.timezone.utc)
    start = end - dt.timedelta(days=args.days)
    query = urllib.parse.urlencode({'project': args.project, 'start': start.replace(tzinfo=None).isoformat(), 'end': end.replace(tzinfo=None).isoformat()})
    with ThreadPoolExecutor(max_workers=3) as pool:
        raw = dict(zip(SENSORS, pool.map(lambda s: get_json(f'{args.api}/sensor/{s}?{query}'), SENSORS)))
    observations, rejected = combine(raw, end)
    if len(observations) < 100:
        raise RuntimeError(f'Only {len(observations)} accepted bins; inspect sensor agreement before fitting')
    cutoff = end - dt.timedelta(days=args.holdout_days)
    training = [o for o in observations if timestamp(o['timestamp']) < cutoff]
    validation = [o for o in observations if timestamp(o['timestamp']) >= cutoff]
    if len(training) < 50 or len(validation) < 20:
        raise RuntimeError('Insufficient chronological training/validation observations')
    weather_query = urllib.parse.urlencode({'latitude':38.91088,'longitude':-76.98569,'hourly':'precipitation', 'start_date':start.date().isoformat(),'end_date':end.date().isoformat(),'timezone':'GMT'})
    weather = get_json('https://api.open-meteo.com/v1/forecast?' + weather_query)
    hourly = weather['hourly']
    rain = []
    for t, mm in zip(hourly['time'], hourly['precipitation']):
        begin = timestamp(t)
        finish = begin + dt.timedelta(hours=1)
        if finish <= start or begin >= end:
            continue
        if mm is None or not math.isfinite(float(mm)):
            raise RuntimeError('Missing rainfall: refusing an unforced experiment')
        rain.append([serial(begin), serial(finish), float(mm)/1000])
    if not rain or rain[0][0] > serial(start) or rain[-1][1] < serial(end):
        raise RuntimeError('Rainfall does not cover the experiment window')
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    (output/'raw_sensors.json').write_text(json.dumps(raw))
    (output/'weather.json').write_text(json.dumps(weather))
    (output/'observations.json').write_text(json.dumps({'accepted':observations,'rejected':rejected},indent=2))
    data = {'start':serial(start),'end':serial(end),'train_end':serial(cutoff),
            'initial_index':training[0]['value'], 'observation_sigma':statistics.median(o['sigma'] for o in training),
            'rain':rain,'training':[[o['ohq_serial'],o['value']] for o in training],
            'validation':[[o['ohq_serial'],o['value']] for o in validation],
            'source':'Recorded field A0/A1/A2, Cell 1; normalized dry/wet index, not calibrated VWC',
            'start_utc':start.isoformat(),'end_utc':end.isoformat(),'train_end_utc':cutoff.isoformat()}
    (output/'input.json').write_text(json.dumps(data,indent=2))
    print(json.dumps({'accepted_bins':len(observations),'rejected_bins':len(rejected),'training':len(training),'holdout':len(validation),'cutoff':cutoff.isoformat(),'raw_counts':{s:len(r) for s,r in raw.items()}},indent=2))


if __name__ == '__main__':
    main()
