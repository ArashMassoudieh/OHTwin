# JM reduced-model experiment results

Recorded sensor period: 2026-11-03T17:15:00+00:00 to 2026-11-17T17:15:00+00:00.
Training ends at 2026-11-14T17:15:00+00:00; final three days are withheld.

All errors below are percentage points of a normalized dry/wet index, not volumetric water content.

| Prediction | Training RMSE | Withheld RMSE | Withheld bias |
|---|---:|---:|---:|
| Uncalibrated model | 67.76 | 88.59 | -88.37 |
| Calibrated model | 9.37 | 24.47 | -23.69 |
| Last-observation persistence | — | 6.20 | -0.31 |

Calibration does not beat persistence on this withheld period.
The reduced observation/model combination is an experiment, not a validated replacement for the live twin.

Accepted bins: 1324; rejected bins: 20.
Probe usage among accepted bins: {"staging_moisture_sensor": 1089, "staging_moisture_a1": 1324, "staging_moisture_a2": 243}.

Shared infiltration rate: 13.1546/day; shared drainage rate: 0.0666002/day.
These are effective reservoir rates, not measured soil hydraulic properties.

Interpretation and limits:

- All three probes contribute to one Cell 1 observation; the consensus pair can change and create abrupt steps.
- A2 is frequently outside the consensus. This alone does not establish a sensor fault.
- Cells 2 and 3 use shared rates and receive no sensor observations.
- The GA fits training observations only; holdout readings are used solely for scoring.
- API weather from the holdout period is supplied, so this is a conditional hindcast, not an operational forecast validation.
- This test uses parameter calibration, not a Kalman/state update or a streaming Bayesian posterior.
- Water-level mappings, soil-specific calibration and probe depths remain unresolved.
- Before promotion, establish a representative observation operator and validate on a new withheld period or rainfall event.

Detailed metrics, raw inputs, GA log and comparison figure are saved beside this report.

Full saved run: `outputs/run-20261117/` (generated data and plot are not tracked by Git).
