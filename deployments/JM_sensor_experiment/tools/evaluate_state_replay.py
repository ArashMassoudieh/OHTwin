#!/usr/bin/env python3
"""Score causal state updates against frozen parameters and same-origin persistence."""
import argparse
import csv
import json
import os
from pathlib import Path
import numpy as np
from evaluate import metrics, read_output


def score(run, origin_start=None):
    data=json.loads((run/'input.json').read_text())
    truth=np.array(data['validation'])
    frozen=read_output(run/'assimilated.csv')['Soil 1 moisture index']
    with (run/'rolling_forecasts.csv').open() as f:
        forecasts=[{k:float(v) for k,v in r.items()} for r in csv.DictReader(f)]
    if origin_start is not None:
        forecasts=[r for r in forecasts if r['origin']>=origin_start-1e-8]
    scores={};increments={}
    for r in forecasts:
        if r['observation_available_at']>r['origin']+1e-8:
            raise RuntimeError('Future observation used in state update')
        if not 0<=r['updated_index']<=100:
            raise RuntimeError('Unbounded soil state')
        increments[r['origin']]=r['state_increment_m3']
    for horizon in [1,6,24,48,72]:
        group=[]
        for r in forecasts:
            if r['horizon_hours']!=horizon:continue
            t=r['target'];index=np.searchsorted(truth[:,0],t)
            if index==0 or index==len(truth):continue
            if truth[index,0]-truth[index-1,0]>1/48+1e-8:continue
            observed=float(np.interp(t,truth[:,0],truth[:,1]))
            group.append((observed,r['prediction'],r['persistence'],float(np.interp(t,frozen[:,0],frozen[:,1]))))
        if not group:
            scores[str(horizon)]={'n':0,'note':'Insufficient future observations; no score'}
            continue
        z=np.array(group)
        scores[str(horizon)]={'n':len(z),'state_updated':metrics(z[:,1],z[:,0]),
                              'persistence':metrics(z[:,2],z[:,0]),'frozen_parameters':metrics(z[:,3],z[:,0])}
    return {'horizon_hours':scores,'state_update_count':len(increments),
            'net_analysis_increment_m3':sum(increments.values()),
            'absolute_analysis_increment_m3':sum(abs(v) for v in increments.values()),
            'gain':data['state_update_gain'],'parameters_fit_only_before':data['train_end_utc'],
            'classification':'Retrospective conditional hindcast, observed API rain, provisional index-to-storage mapping',
            'truth_alignment':'Linear interpolation between adjacent 15-minute medians, gaps over 30 minutes excluded',
            'updates':'Cell 1 soil only; six-hour cadence; observation available only at bin end; volumes logged explicitly'}


def main():
    ap=argparse.ArgumentParser();ap.add_argument('run',type=Path);args=ap.parse_args();p=args.run.resolve()
    results={mode:score(p/mode) for mode in ['a0','a2']}
    (p/'state_metrics.json').write_text(json.dumps(results,indent=2)+'\n')
    lines=['# Raw-data replay and soil-state update test','','**Retrospective diagnostic experiment. No live replacement or independent forecasting-skill claim.**',
           '', 'Raw voltage period: 10 November 2026 21:45 UTC to 17 November 17:15 UTC. Training ends 14 November 17:15 UTC. Fixed per-probe conversion; no historical percentage series used.',
           '', 'The observation-to-storage relation remains provisional. Parameters fit the training period only; subsequent six-hourly updates correct Cell 1 soil storage by half the difference from the latest available observation. Pond and Cells 2/3 are carried forward without direct observation corrections.',
           '', '| Probe target | Horizon | Forecasts | State-updated RMSE | Frozen-model RMSE | Persistence RMSE |',
           '|---|---:|---:|---:|---:|---:|']
    for mode,result in results.items():
        for h,m in result['horizon_hours'].items():
            if not m['n']:continue
            vals=[m[k]['rmse_percentage_points'] for k in ['state_updated','frozen_parameters','persistence']]
            lines.append(f"| {mode.upper()} | {h} h | {m['n']} | {vals[0]:.2f} | {vals[1]:.2f} | {vals[2]:.2f} |")
    lines += ['', 'Errors are normalized-index percentage points, not volumetric water content. A0 and A2 are separate targets and diagnostic controls, not duplicate measurements of a verified bulk-soil state.',
              '', '## Interpretation and limits', '', '- These test dates were previously inspected. The short raw-data record cannot establish independent validation or 72-hour performance; no 72-hour scores are available.',
              '- This is a fixed-gain state update, not an EnKF or Bayesian filter. The gain 0.5 is a declared demonstration setting and was not optimized against the test period.',
              '- Forecasts use historical API rainfall. They are conditional hindcasts, not operational weather-forecast validation.',
              '- Storage corrections introduce/remove water. Their signed and absolute volumes are recorded; they are analysis increments, not measured infiltration or rainfall.',
              '- No noise threshold was used to discard A1 automatically. Its transitions are diagnosed separately in preparation.json.',
              '- Exact-time initialization artifacts remain omitted. Causality and correction-volume tests check the state-update implementation.',
              '', '## State-correction volume']
    for mode,result in results.items():
        lines.append(f"- {mode.upper()}: {result['state_update_count']} updates; net {result['net_analysis_increment_m3']:.4f} m³; total absolute correction {result['absolute_analysis_increment_m3']:.4f} m³.")
    (p/'RESULTS.md').write_text('\n'.join(lines)+'\n')
    print('\n'.join(lines))
    os.environ.setdefault('MPLCONFIGDIR',str(p/'mpl-cache'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig,axes=plt.subplots(1,2,figsize=(11,4.5),constrained_layout=True)
    for ax,(mode,result) in zip(axes,results.items()):
        hs=[int(h) for h,m in result['horizon_hours'].items() if m['n']]
        for key,label,color in [('frozen_parameters','Frozen model','#94a3b8'),('state_updated','State updates','#2563eb'),('persistence','Persistence','#d97706')]:
            ax.plot(hs,[result['horizon_hours'][str(h)][key]['rmse_percentage_points'] for h in hs],marker='o',label=label,color=color)
        ax.set_title(mode.upper()+' target');ax.set_xlabel('Forecast horizon (hours)');ax.set_ylabel('RMSE (index percentage points)');ax.grid(alpha=.2);ax.legend()
    fig.suptitle('Raw-voltage replay: causal soil-state updates\nRetrospective conditional hindcast; provisional calibration')
    fig.savefig(p/'state_comparison.png',dpi=160);plt.close(fig)

if __name__=='__main__':main()
