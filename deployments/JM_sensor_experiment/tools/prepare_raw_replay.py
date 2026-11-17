#!/usr/bin/env python3
"""Prepare a stable, explicit raw-voltage calibration epoch; no network or gap filling."""
import argparse
import datetime as dt
import json
import math
from pathlib import Path
import statistics
from prepare_data import timestamp, serial, SENSORS, BIN_SECONDS

CALIBRATION = {SENSORS[0]:(17155,5600),SENSORS[1]:(17155,2400),SENSORS[2]:(17155,6400)}


def voltage_to_index(volts, dry, wet):
    if not math.isfinite(volts) or not dry > wet:
        raise ValueError('Invalid voltage or calibration')
    return min(100.,max(0.,100*(dry-volts*32768/4.096)/(dry-wet)))


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('raw',type=Path);ap.add_argument('reference',type=Path);ap.add_argument('output',type=Path)
    args=ap.parse_args()
    reference=json.loads((args.reference/'input.json').read_text())
    end=timestamp(reference['end_utc'])
    rows={s:json.loads((args.raw/(s+'_raw.json')).read_text()) for s in SENSORS}
    first=max(min(timestamp(r['timestamp']) for r in values) for values in rows.values())
    start=dt.datetime.fromtimestamp((int(first.timestamp())//BIN_SECONDS+1)*BIN_SECONDS,dt.timezone.utc)
    bins={}
    for sensor,values in rows.items():
        for row in values:
            t=timestamp(row['timestamp']);v=float(row['value'])
            if not math.isfinite(v) or not start<=t<end:continue
            key=int(t.timestamp())//BIN_SECONDS
            if (key+1)*BIN_SECONDS>end.timestamp():continue
            bins.setdefault(key,{}).setdefault(sensor,[]).append(v)
    args.output.mkdir(parents=True,exist_ok=False)
    diagnostics={'calibration_counts':CALIBRATION,'raw_start':start.isoformat(), 'raw_end':end.isoformat(),
                 'classification':'Retrospective stable-calibration replay; same inspected test dates',
                 'raw_unit':'V; older records rounded to 0.01 V, new records retain precision after the 2026-11-17 uploader update','channels':{}}
    for key,sensor in [('a0',SENSORS[0]),('a2',SENSORS[2])]:
        target=args.output/key;target.mkdir()
        accepted=[]
        for bin_id,channels in sorted(bins.items()):
            if sensor not in channels or len(channels[sensor])<5:continue
            volts=statistics.median(channels[sensor])
            t=dt.datetime.fromtimestamp((bin_id+.5)*BIN_SECONDS,dt.timezone.utc)
            value=voltage_to_index(volts,*CALIBRATION[sensor])
            accepted.append({'timestamp':t.isoformat(),'ohq_serial':serial(t),'value':value,'sigma':5.,
                             'per_probe':{sensor:value},'used_probes':[sensor],'excluded_probes':[], 'raw_median_v':volts})
        data=dict(reference)
        data.update(start=serial(start),start_utc=start.isoformat(),observation_sigma=5.,
                    source=f'{sensor}_raw: fixed ADC dry/wet endpoints {CALIBRATION[sensor]}; provisional index, not VWC',
                    evaluation_status='Retrospective raw-voltage replay; previously inspected test period',
                    rolling_state_test=True,state_update_gain=.5)
        data['training']=[[o['ohq_serial'],o['value']] for o in accepted if o['ohq_serial']<data['train_end']]
        data['validation']=[[o['ohq_serial'],o['value']] for o in accepted if o['ohq_serial']>=data['train_end']]
        if len(data['training'])<50 or len(data['validation'])<50:raise RuntimeError('Insufficient observations')
        data['initial_index']=data['training'][0][1]
        (target/'input.json').write_text(json.dumps(data,indent=2)+'\n')
        (target/'observations.json').write_text(json.dumps({'accepted':accepted,'rejected':[]},indent=2)+'\n')
        diagnostics['channels'][key]={'training':len(data['training']),'validation':len(data['validation']),'sensor':sensor}
    # A1 transitions: classify duration, without deciding which level is physically correct.
    a1=[]
    for k,c in sorted(bins.items()):
        if SENSORS[1] in c and len(c[SENSORS[1]])>=5:
            a1.append((k,statistics.median(c[SENSORS[1]])))
    jumps=[]
    for i in range(1,len(a1)):
        k,v=a1[i];prior_k,prior=a1[i-1]
        if k-prior_k!=1 or abs(v-prior)<.1:continue
        reverted=None
        for j in range(i+1,min(len(a1),i+9)):
            if abs(a1[j][1]-prior)<=.03:
                reverted=(a1[j][0]-k)*15;break
        jumps.append({'utc':dt.datetime.fromtimestamp(k*BIN_SECONDS,dt.timezone.utc).isoformat(),
                      'before_v':prior,'after_v':v,'return_within_0_03_v_minutes':reverted})
    diagnostics['a1_changes_at_least_0_1_v']=jumps
    (args.output/'preparation.json').write_text(json.dumps(diagnostics,indent=2)+'\n')
    print(json.dumps({k:v for k,v in diagnostics.items() if k!='a1_changes_at_least_0_1_v'},indent=2))
    print('A1 large transitions',len(jumps),'return within 2 hours',sum(j['return_within_0_03_v_minutes'] is not None for j in jumps))


if __name__=='__main__':main()
