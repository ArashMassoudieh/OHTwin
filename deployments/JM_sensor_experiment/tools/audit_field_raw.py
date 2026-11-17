#!/usr/bin/env python3
"""Read-only field raw-voltage audit; flags are diagnostic, never rewritten readings."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime as dt
import json
import math
from pathlib import Path
import statistics
import urllib.parse
import urllib.request
from prepare_data import SENSORS, timestamp


def summarize(rows, precision_start):
    bins={}
    for row in rows:
        t=timestamp(row['timestamp']);value=float(row['value'])
        if math.isfinite(value):bins.setdefault(int(t.timestamp())//60,[]).append(value)
    series=[(k,statistics.median(v)) for k,v in sorted(bins.items())]
    transitions=[];flags=[]
    for i,(minute,value) in enumerate(series):
        if i and minute==series[i-1][0]+1 and abs(value-series[i-1][1])>=.1:
            prior=series[i-1][1]
            returns=[(k-minute) for k,v in series[i+1:] if 0<k-minute<=10 and abs(v-prior)<=.03]
            transitions.append({'utc':dt.datetime.fromtimestamp(minute*60,dt.timezone.utc).isoformat(),
                                'before_v':prior,'after_v':value,'return_within_10_min':min(returns) if returns else None})
        history=[v for k,v in series[max(0,i-5):i] if 0<minute-k<=5]
        if len(history)>=3:
            center=statistics.median(history);mad=statistics.median(abs(v-center) for v in history)
            threshold=max(.05,6*1.4826*mad)
            if abs(value-center)>threshold:
                flags.append({'utc':dt.datetime.fromtimestamp(minute*60,dt.timezone.utc).isoformat(),'value_v':value,'trailing_center_v':center,'threshold_v':threshold})
    values=[v for _,v in series]
    precise=[r for r in rows if timestamp(r['timestamp'])>=precision_start and abs(float(r['value'])-round(float(r['value']),2))>1e-8]
    return {'records':len(rows),'minute_bins':len(series),'min_v':min(values) if values else None,
            'max_v':max(values) if values else None,'large_adjacent_changes':transitions,
            'causal_diagnostic_flags':flags,'new_records_beyond_two_decimals':len(precise),
            'first':rows[0]['timestamp'] if rows else None,'last':rows[-1]['timestamp'] if rows else None}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--start',required=True)
    ap.add_argument('--precision-start',default='2026-11-17T18:17:35Z')
    args=ap.parse_args()
    end=dt.datetime.now(dt.timezone.utc)
    end=dt.datetime.fromtimestamp(int(end.timestamp())//900*900,dt.timezone.utc)
    query=urllib.parse.urlencode({'project':'John-McCormack','start':timestamp(args.start).replace(tzinfo=None).isoformat(),'end':end.replace(tzinfo=None).isoformat()})
    def get(sensor):
        with urllib.request.urlopen('http://54.213.147.59:5000/sensor/'+sensor+'_raw?'+query,timeout=45) as response:
            return sensor,json.load(response)
    args.output.mkdir(parents=True,exist_ok=False)
    result={'start':timestamp(args.start).isoformat(),'end':end.isoformat(),'channels':{},
            'flags_policy':'Diagnostic only; do not overwrite raw measurements or classify all shifts as noise',
            'no_data_rewriting':True}
    with ThreadPoolExecutor(max_workers=3) as pool:
        for sensor,rows in pool.map(get,SENSORS):
            rows.sort(key=lambda r:r['timestamp'])
            (args.output/(sensor+'_raw.json')).write_text(json.dumps(rows)+'\n')
            result['channels'][sensor]=summarize(rows,timestamp(args.precision_start))
    (args.output/'signal_audit.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
