# Field follow-up: raw signals, frozen model and lab configuration

New-data window: 2026-11-17T17:15:00+00:00 through 2026-11-17T20:00:00+00:00.

**Decision: keep the live model. The follow-up window is too short to validate a replacement.**

## Field raw data

| Probe | Minute bins | Range (V) | Adjacent changes ≥0.1 V | New readings beyond 0.01 V precision |
|---|---:|---:|---:|---:|
| A0 | 165 | 0.918250–0.930000 | 0 | 103 |
| A1 | 165 | 0.381750–0.871000 | 9 | 103 |
| A2 | 165 | 1.314000–1.340000 | 0 | 100 |

A1 continues to show abrupt voltage transitions after the precision update. A0/A2 remain smooth in this window. This rules out uploader rounding as the complete explanation, but does not distinguish probe/wiring/power effects from acquisition issues.

A trailing median/MAD rule flags unusual readings for diagnostics only. It does not overwrite raw values, remove sustained shifts, alter field calibration or change live alarms. Return-time annotations use subsequent data and are retrospective; causal flags use past readings only.

## Frozen-parameter follow-up

Previously fitted parameters, gain 0.5 and six-hour update cadence were frozen; there was no parameter refit. Only forecast origins in the new-data window are scored. Historical forcing already used by the prior replay was kept unchanged; newly needed rainfall hours were appended. The forecast uses API weather, so it remains a conditional hindcast.

| Target | Horizon | Forecasts | State-update RMSE | Persistence RMSE |
|---|---:|---:|---:|---:|
| A0 | 1 h | 1 | 0.593 | 0.608 |
| A2 | 1 h | 1 | 0.629 | 1.671 |

Errors are provisional dry/wet-index points. There is only one new forecast origin per probe and insufficient data for longer horizons. No claim of independent rainfall-event or operational forecasting skill is justified. The water-storage observation mapping still needs physical calibration.

## Completed operational work

- Field raw-voltage precision is deployed and verified in the cloud. The field sensor configuration was not changed.
- Archived 12 inactive lab entries for ADS moisture/pressure and both MaxBotix paths. These sensors were moved to the field; their absence is not evidence of a lab hardware fault.
- Lab active sensors are its connected HC and BMP390 pressure/temperature. Old entries and configuration backups are retained.

## Remaining physical limitation

A1 cannot yet be certified as a reliable soil observation. Check its probe/cable/connectors, common ground and power against A0/A2 at the field station; retain the raw history. Do not change wiring based solely on these flags.

A longer independent period including rain must arrive before validating a replacement. No live model replacement was made.

Validation: all 16 focused tests passed. Live field and lab health were verified after the lab inventory correction.
