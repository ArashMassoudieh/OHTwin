#!/usr/bin/env python3
"""Score untouched holdout data, including a persistence control; plot provenance."""
import argparse
import csv
import datetime as dt
import json
import os
from pathlib import Path
import numpy as np

EPOCH=dt.datetime(1899,12,30,tzinfo=dt.timezone.utc)
def dates(values):return [EPOCH+dt.timedelta(days=float(v)) for v in values]

def read_output(path):
    with path.open() as f:
        rows=csv.reader(f);header=next(rows)
        data=np.array([[float(v) if v else np.nan for v in row] for row in rows])
    return {header[i].strip():data[:,i-1:i+1][np.isfinite(data[:,i-1])&np.isfinite(data[:,i])] for i in range(1,len(header),2)}

def metrics(predicted,observed):
    errors=predicted-observed
    variance=np.sum((observed-np.mean(observed))**2)
    return {'rmse_percentage_points':float(np.sqrt(np.mean(errors**2))),
            'mae_percentage_points':float(np.mean(abs(errors))),
            'bias_percentage_points':float(np.mean(errors)),
            'nse':float(1-np.sum(errors**2)/variance) if variance else None,
            'n':len(observed)}

def main():
    ap=argparse.ArgumentParser();ap.add_argument('run',type=Path);args=ap.parse_args();p=args.run.resolve()
    os.environ.setdefault('MPLCONFIGDIR',str(p/'mpl-cache'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
    d=json.load(open(p/'input.json'));obs=json.load(open(p/'observations.json'));params=json.load(open(p/'parameters.json'))
    output={name:read_output(p/(name+'.csv')) for name in ['baseline','assimilated']}
    scores={}
    for name,series in output.items():
        xy=series['Soil 1 moisture index'];scores[name]={}
        for period in ['training','validation']:
            z=np.array(d[period]);pred=np.interp(z[:,0],xy[:,0],xy[:,1]);scores[name][period]=metrics(pred,z[:,1])
        for label,xy in series.items():
            if label.endswith('moisture index') and (np.min(xy[:,1]) < -1e-3 or np.max(xy[:,1]) > 100.001):
                raise RuntimeError(f'Invalid moisture storage bounds in {name}: {label}')
    z=np.array(d['validation']);persistence=d['training'][-1][1]
    scores['persistence']={'validation':metrics(np.full(len(z),persistence),z[:,1]),'value':persistence}
    rmse0=scores['baseline']['validation']['rmse_percentage_points'];rmse1=scores['assimilated']['validation']['rmse_percentage_points']
    scores['holdout_improvement_vs_uncalibrated_percent']=100*(rmse0-rmse1)/rmse0
    scores['beats_persistence']=rmse1<scores['persistence']['validation']['rmse_percentage_points']
    scores['quality']={'accepted_bins':len(obs['accepted']),'rejected_bins':len(obs['rejected']),
        'probe_bins_used':{s:sum(s in o['used_probes'] for o in obs['accepted']) for s in ['staging_moisture_sensor','staging_moisture_a1','staging_moisture_a2']}}
    scores['method']=params['method'];scores['holdout_used_for_fitting']=False
    scores['evaluation_type']=d.get('evaluation_status', 'conditional hindcast using API weather for the withheld period')
    scores['observation_source']=d.get('source', '')
    (p/'metrics.json').write_text(json.dumps(scores,indent=2)+'\n')
    report=['# JM reduced-model experiment results',
        '',f"Recorded sensor period: {d['start_utc']} to {d['end_utc']}.",
        f"Training ends at {d['train_end_utc']}; final three days are withheld.",
        '', f"Observation source: {d.get('source', 'recorded field observations')}.",
        f"Evaluation status: {scores['evaluation_type']}.",
        '', 'All errors below are percentage points of a normalized dry/wet index, not volumetric water content.',
        '', '| Prediction | Training RMSE | Withheld RMSE | Withheld bias |',
        '|---|---:|---:|---:|']
    for name,label in [('baseline','Uncalibrated model'),('assimilated','Calibrated model')]:
        a=scores[name];report.append(f"| {label} | {a['training']['rmse_percentage_points']:.2f} | {a['validation']['rmse_percentage_points']:.2f} | {a['validation']['bias_percentage_points']:.2f} |")
    a=scores['persistence']['validation'];report.append(f"| Last-observation persistence | — | {a['rmse_percentage_points']:.2f} | {a['bias_percentage_points']:.2f} |")
    report += ['',f"Calibration {'beats' if scores['beats_persistence'] else 'does not beat'} persistence on this withheld period.",
        'The reduced observation/model combination is an experiment, not a validated replacement for the live twin.',
        '',f"Accepted bins: {len(obs['accepted'])}; rejected bins: {len(obs['rejected'])}.",
        f"Probe usage among accepted bins: {json.dumps(scores['quality']['probe_bins_used'])}.",
        '',f"Shared infiltration rate: {params['infiltration_rate_per_day']:.6g}/day; shared drainage rate: {params['drainage_rate_per_day']:.6g}/day.",
        'These are effective reservoir rates, not measured soil hydraulic properties.',
        '', 'Interpretation and limits:',
        '', '- Only Cell 1 is observed; the observation definition is recorded above and in input.json.',
        '- Probe disagreement alone does not establish a sensor fault; installation depths and calibration remain unresolved.',
        '- Cells 2 and 3 use shared rates and receive no sensor observations.',
        '- The GA fits training observations only; holdout readings are used solely for scoring.',
        '- API weather from the holdout period is supplied, so this is a conditional hindcast, not an operational forecast validation.',
        '- This test uses parameter calibration, not a Kalman/state update or a streaming Bayesian posterior.',
        '- Water-level mappings, soil-specific calibration and probe depths remain unresolved.',
        '- Before promotion, establish a representative observation operator and validate on a new withheld period or rainfall event.',
        '', 'Detailed metrics, raw inputs, GA log and comparison figure are saved beside this report.']
    (p/'RESULTS.md').write_text('\n'.join(report)+'\n')
    plt.rcParams.update({'font.size':10,'axes.spines.top':False,'axes.spines.right':False})
    fig,ax=plt.subplots(3,1,figsize=(12,10),sharex=True,constrained_layout=True)
    colors=['#2563eb','#16a34a','#d97706']
    for s,color,label in zip(['staging_moisture_sensor','staging_moisture_a1','staging_moisture_a2'],colors,['A0','A1','A2']):
        rows=[o for o in obs['accepted']+obs['rejected'] if s in o['per_probe']];rows.sort(key=lambda o:o['timestamp'])
        ax[0].plot([dt.datetime.fromisoformat(o['timestamp']) for o in rows],[o['per_probe'][s] for o in rows],color=color,alpha=.6,lw=1,label=label)
    accepted=obs['accepted'];ax[0].plot([dt.datetime.fromisoformat(o['timestamp']) for o in accepted],[o['value'] for o in accepted],color='#111827',lw=1.5,label='Combined observation')
    ax[0].set_title('Recorded Cell 1 probes and the quality-controlled combined observation',loc='left');ax[0].set_ylabel('Dry/wet index (%)');ax[0].legend(ncol=4,loc='lower left')
    for name,color,label in [('baseline','#94a3b8','Uncalibrated model'),('assimilated','#2563eb','Calibrated model')]:
        xy=output[name]['Soil 1 moisture index'];ax[1].plot(dates(xy[:,0]),xy[:,1],color=color,lw=1.7,label=label)
    allobs=np.array(d['training']+d['validation']);ax[1].scatter(dates(allobs[:,0]),allobs[:,1],s=4,color='#111827',alpha=.5,label='Observed')
    ax[1].plot(dates(z[:,0]),np.full(len(z),persistence),'--',color='#a855f7',label='Persistence control')
    ax[1].set_ylabel('Dry/wet index (%)');ax[1].set_title('Cell 1: fit on earlier data; freeze parameters before the final three days',loc='left');ax[1].legend(ncol=2,loc='lower left')
    for i,color in zip([1,2,3],colors):
        xy=output['assimilated'][f'Soil {i} moisture index'];ax[2].plot(dates(xy[:,0]),xy[:,1],color=color,ls='-' if i==1 else '--',label=f'Cell {i}'+(' (calibration target)' if i==1 else ' (model only)'))
    ax[2].set_title('Shared parameters across three cells; Cells 2 and 3 have no assimilated observations',loc='left');ax[2].set_ylabel('Dry/wet index (%)');ax[2].legend(ncol=3,loc='lower left')
    cutoff=dates([d['train_end']])[0]
    for a in ax:
        a.axvspan(cutoff,dates([d['end']])[0],color='#f59e0b',alpha=.1);a.axvline(cutoff,color='#92400e',ls=':',lw=1);a.grid(alpha=.15);a.set_ylim(-2,105)
    ax[-1].xaxis.set_major_formatter(mdates.DateFormatter('%b %d',tz=dt.timezone.utc));ax[-1].set_xlabel('2026 — UTC dates; shaded area is withheld validation')
    fig.suptitle('JM reduced-model assimilation experiment\nReal sensor observations; effective storage rates; no synthetic observation noise',fontsize=14)
    fig.savefig(p/'comparison.png',dpi=160);plt.close(fig)
    print(json.dumps(scores,indent=2))

if __name__=='__main__':main()
