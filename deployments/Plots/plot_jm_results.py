#!/usr/bin/env python3
from __future__ import annotations
import argparse,csv,json,math,re
from dataclasses import dataclass
from datetime import datetime,timedelta
from pathlib import Path
from typing import Optional
import matplotlib
matplotlib.use('Agg')
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
@dataclass
class Series: name:str; time:list[float]; values:list[float]
@dataclass
class Dataset: path:Path; headers:list[str]; series:dict[str,Series]; minimum_time:float; maximum_time:float
def clean(s): return re.sub(r'\s+',' ',re.sub(r'\([^)]*\)',' ',s.strip()).replace('_',' ')).strip()
def canon(s): return clean(s).lower()
def ff(v):
    try:
        x=float(str(v).strip()); return x if math.isfinite(x) else float('nan')
    except Exception:return float('nan')
def delim(path):
    sample=path.read_text(encoding='utf-8',errors='replace')[:16384]
    try:return csv.Sniffer().sniff(sample,delimiters=',;\t').delimiter
    except csv.Error:return ','
def to_dt(v): return datetime(1899,12,30)+timedelta(days=v)
def plot_time(vals):
    finite=[v for v in vals if math.isfinite(v)]
    if not finite:return vals
    med=sorted(finite)[len(finite)//2]
    if 20000<=med<=80000:return [to_dt(v) if math.isfinite(v) else None for v in vals]
    if med>100_000_000:return [datetime.fromtimestamp(v) if math.isfinite(v) else None for v in vals]
    return vals
def read_csv(path):
    with path.open(newline='',encoding='utf-8',errors='replace') as f: rows=list(csv.reader(f,delimiter=delim(path)))
    if not rows: raise ValueError(f'Empty CSV: {path}')
    headers=rows[0]; data=[r for r in rows[1:] if r]; series={}
    for j,h in enumerate(headers):
        name=clean(h)
        if not name or canon(name) in {'t','time','date','datetime','timestamp'}: continue
        ti=max(0,j-1); pairs=[]
        for row in data:
            t=ff(row[ti] if ti<len(row) else 'nan'); v=ff(row[j] if j<len(row) else 'nan')
            if math.isfinite(t) and math.isfinite(v): pairs.append((t,v))
        if pairs: series[canon(name)]=Series(name,[p[0] for p in pairs],[p[1] for p in pairs])
    times=[t for s in series.values() for t in s.time]
    if not times: raise ValueError(f'No valid time series found in {path}')
    return Dataset(path,headers,series,min(times),max(times))
def exact(ds,name): return ds.series.get(canon(name))
def grouped(ds,pattern):
    rx=re.compile(pattern,re.I); out=[]
    for s in ds.series.values():
        m=rx.fullmatch(s.name.strip())
        if m: out.append((int(m.group(1)),s))
    return sorted(out)
def match(assim,s): return None if assim is None else assim.series.get(canon(s.name))
def style(ax,isdt):
    ax.grid(True,alpha=.25)
    if isdt:
        loc=mdates.AutoDateLocator(minticks=4,maxticks=8); ax.xaxis.set_major_locator(loc); ax.xaxis.set_major_formatter(mdates.ConciseDateFormatter(loc))
def one(ax,s,assim,label=None):
    ax.plot(plot_time(s.time),s.values,lw=1.35,label=label or s.name)
    a=match(assim,s)
    if a: ax.plot(plot_time(a.time),a.values,lw=1.15,ls='--',label=f'{label or s.name} — assimilation')
def save_group(path,items,assim,title,ylabel,xlim=None):
    if not items:return
    fig,ax=plt.subplots(figsize=(12,5.5),constrained_layout=True); isdt=isinstance(plot_time([items[0][1].time[0]])[0],datetime)
    for lab,s in items: one(ax,s,assim,lab)
    ax.set(title=title,xlabel='Time',ylabel=ylabel); style(ax,isdt)
    if xlim: ax.set_xlim(*xlim)
    ax.legend(loc='best',frameon=False,ncol=2); fig.savefig(path,dpi=240,bbox_inches='tight'); plt.close(fig); print(f'[ok] wrote {path}')
def save_stack(path,panels,assim,title,xlim=None):
    if not panels:return
    fig,axes=plt.subplots(len(panels),1,figsize=(12,max(3*len(panels),5)),sharex=True,constrained_layout=True)
    if len(panels)==1: axes=[axes]
    isdt=isinstance(plot_time([panels[0][1].time[0]])[0],datetime)
    for ax,(lab,s) in zip(axes,panels):
        one(ax,s,assim,'Truth'); ax.set_ylabel(lab); style(ax,isdt)
        if xlim: ax.set_xlim(*xlim)
        if match(assim,s): ax.legend(loc='best',frameon=False)
    axes[-1].set_xlabel('Time'); fig.suptitle(title,fontsize=14); fig.savefig(path,dpi=240,bbox_inches='tight'); plt.close(fig); print(f'[ok] wrote {path}')
def window(ds,days,late):
    w=min(days,max(0,ds.maximum_time-ds.minimum_time)); raw=[ds.maximum_time-w,ds.maximum_time] if late else [ds.minimum_time,ds.minimum_time+w]; p=plot_time(raw); return raw,(p[0],p[1])
def main():
    p=argparse.ArgumentParser(); p.add_argument('--truth',type=Path,required=True); p.add_argument('--assimilation',type=Path); p.add_argument('--output-dir',type=Path,default=Path('JM_plot_results')); p.add_argument('--window-days',type=float,default=10); a=p.parse_args()
    truth=read_csv(a.truth); assim=read_csv(a.assimilation) if a.assimilation else None; a.output_dir.mkdir(parents=True,exist_ok=True)
    ponds=grouped(truth,r'Pond\s+(\d+)\s+water\s+depth'); soils=grouped(truth,r'Soil\s+(\d+)\s+moisture'); infil=grouped(truth,r'Pond\s+(\d+)\s+infiltration'); over=grouped(truth,r'Pond\s+(\d+)\s+overflow')
    singles={'Precipitation':exact(truth,'Precipitation'),'DA-01 runoff':exact(truth,'DA-01 runoff'),'Gutter 4 depth':exact(truth,'Gutter 4 depth'),'Underdrain outlet flow':exact(truth,'Underdrain outlet flow'),'Groundwater recharge':exact(truth,'Groundwater recharge'),'Catch basin depth':exact(truth,'Catch basin depth'),'Catch basin outlet flow':exact(truth,'Catch basin outlet flow')}
    inv=['# JM plot-variable inventory','',f'- Truth file: `{truth.path}`',f'- Assimilation file: `{assim.path if assim else "disabled or unavailable"}`','','## Truth CSV outputs','']+[f'- `{s.name}`' for s in truth.series.values()]
    for title,items in [('Pond water depths',ponds),('Soil moisture',soils),('Pond infiltration',infil),('Pond overflow',over)]: inv+=['',f'## {title}','']+([f'- {n}: `{s.name}`' for n,s in items] or ['- None detected'])
    inv+=['','## System outputs','']+[f'- {k}: `{v.name if v else "not found"}`' for k,v in singles.items()]
    (a.output_dir/'jm_variable_inventory.md').write_text('\n'.join(inv)+'\n'); print(f'[ok] wrote {a.output_dir/"jm_variable_inventory.md"}')
    suffix='truth_vs_assimilation' if assim else 'truth'
    save_group(a.output_dir/f'paper_jm_pond_depths_{suffix}.png',[(f'Pond {n}',s) for n,s in ponds],assim,'JM pond water depths','Water depth')
    save_group(a.output_dir/f'paper_jm_soil_moisture_{suffix}.png',[(f'Soil {n}',s) for n,s in soils],assim,'JM soil moisture','Moisture')
    save_group(a.output_dir/f'paper_jm_infiltration_{suffix}.png',[(f'Pond {n}',s) for n,s in infil],assim,'JM pond infiltration','Infiltration')
    save_group(a.output_dir/f'paper_jm_overflow_{suffix}.png',[(f'Pond {n}',s) for n,s in over],assim,'JM pond overflow','Overflow')
    flows=[(k,singles[k]) for k in ['Precipitation','DA-01 runoff','Underdrain outlet flow','Groundwater recharge','Catch basin outlet flow'] if singles[k]]
    save_stack(a.output_dir/f'paper_jm_system_flows_{suffix}.png',flows,assim,'JM system fluxes — full period')
    states=[(k,singles[k]) for k in ['Gutter 4 depth','Catch basin depth'] if singles[k]]+[(f'Pond {n} depth',s) for n,s in ponds]
    save_stack(a.output_dir/f'paper_jm_hydraulic_states_{suffix}.png',states,assim,'JM hydraulic states — full period')
    summary=[(k,singles[k]) for k in ['Precipitation','DA-01 runoff','Gutter 4 depth','Catch basin depth','Underdrain outlet flow','Groundwater recharge','Catch basin outlet flow'] if singles[k]]
    if ponds: summary.append((f'Pond {ponds[-1][0]} depth',ponds[-1][1]))
    er,ex=window(truth,a.window_days,False); lr,lx=window(truth,a.window_days,True)
    save_stack(a.output_dir/f'paper_jm_summary_full_{suffix}.png',summary,assim,'JM system summary — full period')
    save_stack(a.output_dir/f'paper_jm_summary_early_{suffix}.png',summary,assim,f'JM system summary — first {a.window_days:g} days',ex)
    save_stack(a.output_dir/f'paper_jm_summary_late_{suffix}.png',summary,assim,f'JM system summary — last {a.window_days:g} days',lx)
    wp=a.output_dir/'jm_plot_windows.json'; wp.write_text(json.dumps({'minimum_time':truth.minimum_time,'maximum_time':truth.maximum_time,'window_days':a.window_days,'early':er,'late':lr},indent=2)+'\n'); print(f'[ok] wrote {wp}')
if __name__=='__main__': main()
