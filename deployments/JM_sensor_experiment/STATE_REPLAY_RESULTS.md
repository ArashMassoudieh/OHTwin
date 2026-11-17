# Raw-data replay and soil-state update test

**Retrospective diagnostic experiment. No live replacement or independent forecasting-skill claim.**

Raw voltage period: 10 November 2026 21:45 UTC to 17 November 17:15 UTC. Training ends 14 November 17:15 UTC. Fixed per-probe conversion; no historical percentage series used.

The observation-to-storage relation remains provisional. Parameters fit the training period only; subsequent six-hourly updates correct Cell 1 soil storage by half the difference from the latest available observation. Pond and Cells 2/3 are carried forward without direct observation corrections.

| Probe target | Horizon | Forecasts | State-updated RMSE | Frozen-model RMSE | Persistence RMSE |
|---|---:|---:|---:|---:|---:|
| A0 | 1 h | 12 | 0.89 | 5.36 | 0.39 |
| A0 | 6 h | 11 | 1.68 | 5.46 | 1.55 |
| A0 | 24 h | 8 | 1.59 | 5.68 | 1.35 |
| A0 | 48 h | 4 | 2.43 | 5.86 | 2.47 |
| A2 | 1 h | 12 | 1.00 | 2.15 | 0.56 |
| A2 | 6 h | 11 | 2.04 | 2.29 | 2.37 |
| A2 | 24 h | 8 | 1.14 | 2.33 | 1.54 |
| A2 | 48 h | 4 | 1.24 | 2.39 | 2.86 |

Errors are normalized-index percentage points, not volumetric water content. A0 and A2 are separate targets and diagnostic controls, not duplicate measurements of a verified bulk-soil state.

## Interpretation and limits

- These test dates were previously inspected. The short raw-data record cannot establish independent validation or 72-hour performance; no 72-hour scores are available.
- This is a fixed-gain state update, not an EnKF or Bayesian filter. The gain 0.5 is a declared demonstration setting and was not optimized against the test period.
- Forecasts use historical API rainfall. They are conditional hindcasts, not operational weather-forecast validation.
- Storage corrections introduce/remove water. Their signed and absolute volumes are recorded; they are analysis increments, not measured infiltration or rainfall.
- No noise threshold was used to discard A1 automatically. Its transitions are diagnosed separately in preparation.json.
- Exact-time initialization artifacts remain omitted. Causality and correction-volume tests check the state-update implementation.

## State-correction volume
- A0: 12 updates; net 0.2497 m³; total absolute correction 0.3779 m³.
- A2: 12 updates; net 0.0970 m³; total absolute correction 0.3682 m³.

## A1 acquisition investigation

A1 has 30 changes of at least 0.1 V between adjacent 15-minute medians; 14 return within 0.03 V of the previous level within two hours. The other transitions do not return within that two-hour search window; this is not proof they persist indefinitely.

The local headless sensor code uses per-sensor timers in the main event loop and synchronous reads. The ADS1115 code selects each channel in single-shot mode and waits for conversion; no concurrent-access defect was established by this source review. Raw values bypass dry/wet conversion, but API values are rounded to 0.01 V. Rounding cannot explain a 0.5 V jump. A wiring/power/probe or acquisition problem remains possible; source review alone cannot localize it, and deployment identity was not checked in this task. No field changes were made.

Validation: 14 focused test cases passed across the suite and added raw-unit checks, including a deterministic test that future observations cannot change an earlier forecast.
