#!/usr/bin/env python3
"""Summarize only forecast origins in the new field follow-up window."""
import argparse
import datetime as dt
import json
from pathlib import Path
from evaluate_state_replay import score
from prepare_data import serial,timestamp


def main():
    ap=argparse.ArgumentParser();ap.add_argument('run',type=Path);args=ap.parse_args();p=args.run.resolve()
    audit=json.loads((p/'signal_audit.json').read_text());cutoff=serial(timestamp(audit['start']))
    scores={mode:score(p/'replay'/mode,origin_start=cutoff) for mode in ['a0','a2']}
    for mode in scores:
        inp=json.loads((p/'replay'/mode/'input.json').read_text())
        params=json.loads((p/'replay'/mode/'parameters.json').read_text())
        assert abs(inp['fixed_parameters'][0]-params['infiltration_rate_per_day'])<1e-10
        assert abs(inp['fixed_parameters'][1]-params['drainage_rate_per_day'])<1e-10
    (p/'followup_metrics.json').write_text(json.dumps(scores,indent=2)+'\n')
    lines=['# Field follow-up: raw signals, frozen model and lab configuration',
           '',f"New-data window: {audit['start']} through {audit['end']}.",
           '', '**Decision: keep the live model. The follow-up window is too short to validate a replacement.**',
           '', '## Field raw data', '', '| Probe | Minute bins | Range (V) | Adjacent changes ≥0.1 V | New readings beyond 0.01 V precision |',
           '|---|---:|---:|---:|---:|']
    for sensor,c in audit['channels'].items():
        label={'staging_moisture_sensor':'A0','staging_moisture_a1':'A1','staging_moisture_a2':'A2'}[sensor]
        lines.append(f"| {label} | {c['minute_bins']} | {c['min_v']:.6f}–{c['max_v']:.6f} | {len(c['large_adjacent_changes'])} | {c['new_records_beyond_two_decimals']} |")
    lines += ['', 'A1 continues to show abrupt voltage transitions after the precision update. A0/A2 remain smooth in this window. This rules out uploader rounding as the complete explanation, but does not distinguish probe/wiring/power effects from acquisition issues.',
              '', 'A trailing median/MAD rule flags unusual readings for diagnostics only. It does not overwrite raw values, remove sustained shifts, alter field calibration or change live alarms. Return-time annotations use subsequent data and are retrospective; causal flags use past readings only.',
              '', '## Frozen-parameter follow-up', '', 'Previously fitted parameters, gain 0.5 and six-hour update cadence were frozen; there was no parameter refit. Only forecast origins in the new-data window are scored. Historical forcing already used by the prior replay was kept unchanged; newly needed rainfall hours were appended. The forecast uses API weather, so it remains a conditional hindcast.',
              '', '| Target | Horizon | Forecasts | State-update RMSE | Persistence RMSE |',
              '|---|---:|---:|---:|---:|']
    for mode,r in scores.items():
        for horizon,m in r['horizon_hours'].items():
            if m['n']:
                lines.append(f"| {mode.upper()} | {horizon} h | {m['n']} | {m['state_updated']['rmse_percentage_points']:.3f} | {m['persistence']['rmse_percentage_points']:.3f} |")
    lines += ['', 'Errors are provisional dry/wet-index points. There is only one new forecast origin per probe and insufficient data for longer horizons. No claim of independent rainfall-event or operational forecasting skill is justified. The water-storage observation mapping still needs physical calibration.',
              '', '## Completed operational work', '', '- Field raw-voltage precision is deployed and verified in the cloud. The field sensor configuration was not changed.',
              '- Archived 12 inactive lab entries for ADS moisture/pressure and both MaxBotix paths. These sensors were moved to the field; their absence is not evidence of a lab hardware fault.',
              '- Lab active sensors are its connected HC and BMP390 pressure/temperature. Old entries and configuration backups are retained.',
              '', '## Remaining physical limitation', '', 'A1 cannot yet be certified as a reliable soil observation. Check its probe/cable/connectors, common ground and power against A0/A2 at the field station; retain the raw history. Do not change wiring based solely on these flags.',
              '', 'A longer independent period including rain must arrive before validating a replacement. No live model replacement was made.']
    (p/'RESULTS.md').write_text('\n'.join(lines)+'\n')
    print('\n'.join(lines))
    import os
    os.environ.setdefault('MPLCONFIGDIR',str(p/'mpl-cache'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
    fig,axes=plt.subplots(3,1,figsize=(11,7),sharex=True,constrained_layout=True)
    for ax,(sensor,details),label in zip(axes,audit['channels'].items(),['A0','A1','A2']):
        rows=json.loads((p/(sensor+'_raw.json')).read_text())
        ax.plot([timestamp(r['timestamp']) for r in rows],[float(r['value']) for r in rows],lw=1,color='#2563eb')
        flagged=details['causal_diagnostic_flags']
        ax.scatter([timestamp(r['utc']) for r in flagged],[r['value_v'] for r in flagged],s=18,color='#dc2626',label='Diagnostic flag')
        ax.set_ylabel(label+' (V)');ax.grid(alpha=.2)
        ax.axvline(timestamp('2026-11-17T18:17:35Z'),ls=':',color='#64748b')
    axes[1].legend(loc='upper left')
    axes[-1].xaxis.set_major_formatter(mdates.DateFormatter('%H:%M',tz=dt.timezone.utc))
    axes[-1].set_xlabel('17 November 2026 — UTC; dotted line: precision update')
    fig.suptitle('Field raw voltages: A1 transitions remain visible\nFlags identify unusual readings; no samples were replaced')
    fig.savefig(p/'raw_signal_comparison.png',dpi=160);plt.close(fig)



if __name__=='__main__':main()
