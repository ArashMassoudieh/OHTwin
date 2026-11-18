#!/usr/bin/env python3
"""Isolated retrospective depth sensitivity; never changes a live deployment."""
import argparse,json,re,shutil,subprocess
from pathlib import Path
from evaluate_state_replay import score

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 ap.add_argument('reference',type=Path);ap.add_argument('output',type=Path)
 args=ap.parse_args();base=Path(__file__).resolve().parents[1]
 out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
 ref=args.reference.resolve();data=json.loads((ref/'a0/input.json').read_text())
 channels=[json.loads((ref/key/'observations.json').read_text())['accepted'] for key in ('a0','a2')]
 maps=[{round(o['ohq_serial'],8):o for o in c} for c in channels]
 pair=[(maps[0][t]['ohq_serial'],(maps[0][t]['value']+maps[1][t]['value'])/2) for t in sorted(maps[0].keys()&maps[1].keys())]
 data['training']=[p for p in pair if p[0]<data['train_end']]
 data['validation']=[p for p in pair if p[0]>=data['train_end']]
 if len(data['training'])<50 or len(data['validation'])<50:
  raise RuntimeError('Insufficient matched A0/A2 observations')
 data['initial_index']=data['training'][0][1]
 data['source']='Fixed A0/A2 equal mean of raw-voltage-derived indices; matched completed 15-minute bins; A1 excluded from fitting due unresolved voltage jumps.'
 data.pop('fixed_parameters',None)
 original=(base/'model/JM.ohq').read_text();results={}
 for label,depth in [('36in_control',.9144),('12in_upper_bound',.3048),('6in_scenario',.1524)]:
  case=out/label;case.mkdir();shutil.copy2(base/'model/jm_reservoirs.json',case)
  capacity=11.3032032*depth*.35
  model=re.sub(r'capacity=3\.617477152128',f'capacity={capacity:.12f}',original)
  if model.count(f'capacity={capacity:.12f}') != 3:
   raise RuntimeError('Expected three updated soil capacities')
  (case/'JM.ohq').write_text(model)
  (case/'input.json').write_text(json.dumps(data,indent=2)+'\n')
  with (case/'runner.log').open('w') as log:
   subprocess.run([str(base/'outputs/experiment'),str(case/'JM.ohq'),str(base.parents[1]/'resources'),str(case/'input.json'),str(case)],stdout=log,stderr=subprocess.STDOUT,check=True)
  params=json.loads((case/'parameters.json').read_text())
  if label=='36in_control':data['fixed_parameters']=[params['infiltration_rate_per_day'],params['drainage_rate_per_day']]
  results[label]={'assumed_depth_m':depth,'capacity_m3':capacity,'scores':score(case)}
  print(label,json.dumps(results[label]['scores']['horizon_hours']),flush=True)
 (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
 (out/'README.md').write_text('# Shallow soil sensitivity\n\nRetrospective diagnostic, not independent validation. All cases use identical fixed-pair A0/A2 targets and chronological split. Parameters fitted only on the 36-inch control training period are frozen for 12-inch and 6-inch scenarios to isolate capacity sensitivity. Twelve inches is an upper bound from user guidance, not a surveyed depth; six inches is an assumed scenario. No buried depths were found in repository history. This simplified model changes effective storage, not a resolved shallow layer coupled to a deep layer. No live model replacement.\n')
if __name__=='__main__':main()
