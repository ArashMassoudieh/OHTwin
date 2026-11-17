#!/usr/bin/env python3
"""Retrospective fixed-membership sensitivity checks, never independent validation."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys

SENSORS = ['staging_moisture_sensor', 'staging_moisture_a1', 'staging_moisture_a2']


def fixed_observation(record, mode):
    values = record['per_probe']
    required = SENSORS if mode == 'fixed_three_mean' else [SENSORS[1]]
    if not all(s in values for s in required):
        return None
    row = dict(record)
    row.update(value=statistics.mean(values[s] for s in required),
               used_probes=required, excluded_probes=[s for s in values if s not in required],
               sigma=5.)
    # Rejected consensus records have no serial time; recover it from UTC.
    time = dt.datetime.fromisoformat(row['timestamp'])
    row['ohq_serial'] = (time - dt.datetime(1899, 12, 30, tzinfo=dt.timezone.utc)).total_seconds()/86400
    row.pop('reason', None)
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[3]
    experiment = Path(__file__).resolve().parents[1]
    source, output = args.source.resolve(), args.output.resolve()
    if output.exists():
        raise RuntimeError('Output already exists; refusing to overwrite a saved experiment')
    output.mkdir(parents=True)
    original = json.loads((source/'input.json').read_text())
    observations = json.loads((source/'observations.json').read_text())
    records = sorted(observations['accepted'] + observations['rejected'], key=lambda r: r['timestamp'])
    transitions = list(zip(observations['accepted'], observations['accepted'][1:]))
    jumps = [(a,b) for a,b in transitions if abs(a['value']-b['value']) > 5]
    diagnostic = {
        'classification': 'Retrospective sensitivity on previously inspected data; not independent validation',
        'source_input_sha256': hashlib.sha256((source/'input.json').read_bytes()).hexdigest(),
        'large_jumps_over_5_points': len(jumps),
        'large_jumps_with_membership_change': sum(set(a['used_probes']) != set(b['used_probes']) for a,b in jumps),
        'rain_total_mm': sum(r[2] for r in original['rain'])*1000,
        'rain_holdout_mm': sum(r[2] for r in original['rain'] if r[0] >= original['train_end'])*1000,
        'modes': {}}
    for mode in ['fixed_three_mean', 'a1_only_control']:
        target = output/mode
        target.mkdir()
        accepted, rejected = [], []
        for row in records:
            fixed = fixed_observation(row, mode)
            if fixed is None:
                rejected.append(dict(row, reason='required fixed-membership probe missing'))
            else:
                accepted.append(fixed)
        data = dict(original)
        data['training'] = [[r['ohq_serial'], r['value']] for r in accepted if r['ohq_serial'] < data['train_end']]
        data['validation'] = [[r['ohq_serial'], r['value']] for r in accepted if r['ohq_serial'] >= data['train_end']]
        if not data['training'] or not data['validation']:
            raise RuntimeError('Missing training or test observations')
        data.update(initial_index=data['training'][0][1], observation_sigma=5.,
                    source=f'{mode}: retrospective observation-operator sensitivity, normalized index only',
                    evaluation_status='Previously inspected holdout; sensitivity only')
        (target/'input.json').write_text(json.dumps(data, indent=2)+'\n')
        (target/'observations.json').write_text(json.dumps({'accepted':accepted,'rejected':rejected}, indent=2)+'\n')
        with (target/'runner.log').open('w') as log:
            subprocess.run([str(experiment/'outputs/experiment'),str(experiment/'model/JM.ohq'),str(root/'resources'),str(target/'input.json'),str(target)],stdout=log,stderr=subprocess.STDOUT,check=True)
        with (target/'evaluation.log').open('w') as log:
            subprocess.run([sys.executable,str(experiment/'tools/evaluate.py'),str(target)],stdout=log,stderr=subprocess.STDOUT,check=True)
        diagnostic['modes'][mode] = json.loads((target/'metrics.json').read_text())
        # Override the generic report so the inspected period cannot be mistaken for a fresh holdout.
        report = target/'RESULTS.md'
        report.write_text('> RETROSPECTIVE SENSITIVITY ONLY: this period was previously inspected. No independent validation claim.\n\n'+report.read_text())
        print(mode, diagnostic['modes'][mode]['assimilated']['validation'], flush=True)
    (output/'sensitivity.json').write_text(json.dumps(diagnostic,indent=2)+'\n')
    print(json.dumps({k:v for k,v in diagnostic.items() if k != 'modes'},indent=2))


if __name__ == '__main__':
    main()
