# Shallow moisture probes: history and model test

User guidance on 2026-11-18: field moisture probes should be less than one foot deep. Exact as-installed depths and probe hardware revisions were not found in the searched repository history. This is a shallow soil constraint, not evidence that sensors observe the entire 0.9144 m engineered layer.

## Calibration provenance

| Commit/date | Evidence |
|---|---|
| `2f438aa`, September 17 | Lab A0 switched to ADS1115; dry air 17155 counts, wet endpoint 6400. |
| `fd22cee`, September 30 | A1 and A2 added to lab ADS1115. |
| `b9d2366`, November 3 | One lab baseline adopted provisionally for all field probes; record explicitly says not volumetric water content and not individual full-cable calibration. |
| `9575c93`, November 10 | Wet endpoints adjusted from observed minima: A0 5600, A1 2400; A2 retained 6400. These are not independent soil calibrations. |
| `c56b96b`, November 10 | Raw voltage telemetry added, enabling a fixed calibration replay across percentage-definition changes. |

Older Sean ADC0804/CD4014 code is a different acquisition path; its ADC counts cannot be reused as ADS1115 endpoints. No confirmed V1.2/V2 identification was established by this search.

## Retrospective shallow storage sensitivity

Fixed A0/A2 equal mean, matched completed 15-minute bins, same raw replay period and chronological split as the November 17 experiment. A1 remains recorded but excluded from this fit because unresolved voltage jumps were demonstrated independently of aggregation. No conclusion that A1 is physically faulty or at a different depth follows.

Fit the 36-inch control on training data only, freeze its parameters and state-update gain, then change effective storage to hypothetical 12-inch and 6-inch depths. Twelve inches is the supplied upper bound; neither scenario is an as-built measurement. All three modeled cells share each assumed depth for this sensitivity experiment; Cells 2/3 remain unobserved.

| Scenario | Horizon (h) | Forecasts | State-update RMSE (points) | Persistence RMSE (points) |
|---|---:|---:|---:|---:|
| 36in_control | 1 | 12 | 0.883 | 0.417 |
| 36in_control | 6 | 11 | 1.810 | 1.919 |
| 36in_control | 24 | 8 | 1.199 | 1.428 |
| 36in_control | 48 | 4 | 1.609 | 2.662 |
| 12in_upper_bound | 1 | 12 | 0.883 | 0.417 |
| 12in_upper_bound | 6 | 11 | 1.810 | 1.919 |
| 12in_upper_bound | 24 | 8 | 1.199 | 1.428 |
| 12in_upper_bound | 48 | 4 | 1.609 | 2.662 |
| 6in_scenario | 1 | 12 | 0.883 | 0.417 |
| 6in_scenario | 6 | 11 | 1.810 | 1.919 |
| 6in_scenario | 24 | 8 | 1.199 | 1.428 |
| 6in_scenario | 48 | 4 | 1.609 | 2.662 |

Reducing capacity barely changes the normalized moisture predictions in this mostly dry interval. The simplified model drains by a first-order rate, so changing capacity alone cannot identify probe depth or fix the observation operator. The apparent 24/48-hour advantage is retrospective, based on only 8/4 overlapping forecasts and previously inspected data, not independent validation. One-hour forecasts are worse than persistence. No 72-hour score is available.

This is not a resolved shallow soil layer exchanging water with a deeper layer. The dry/wet index is still a provisional storage proxy. A physically layered observation model or soil-specific VWC calibration remains necessary for interpreting actual water content. No live twin model, calibration settings, services, or observation targets were changed by this experiment.

HC head/flow remain recorded; leaves in its box limit confidence in using it as forcing. Pressure loop current is retained, but the borrowed lab pressure-depth calibration does not identify field water depth.
