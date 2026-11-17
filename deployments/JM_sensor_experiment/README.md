# JM: reduced three-cell sensor assimilation experiment

This is an isolated test of parameter assimilation using recorded field observations.
It does not replace either live three-cell deployment or change the field Pi.
The observations are real A0/A1/A2 readings; model output is never used as an
observation and no synthetic observation noise is added.

## Observation definition

All three moisture probes are in **Cell 1**. Their current values are provisional
0–100 dry/wet indices, not calibrated volumetric water content. We compare the
combined index with a model observation operator in the same index units.

For each complete UTC 15-minute interval:

1. Take the median of each probe's finite readings within 0–100. Values outside
   the calibrated index range remain in the saved raw data but are not fitted.
2. Give each probe one vote, regardless of its sampling rate.
3. Find the across-probe median; retain probes within 15 percentage points of it.
4. Require at least two probes and no more than 20 points of spread among those
   retained; otherwise omit that interval. Do not forward-fill or interpolate gaps.
5. Use the median of the retained probes as **one Cell 1 moisture observation**.
   Save the included/excluded probe IDs and per-probe values with every interval.
6. Record uncertainty as max(5 points, 1.4826 × median absolute deviation).
   The GA uses the training-period median uncertainty as its common observation
   error scale; this first test does not use a separate likelihood variance per bin.

These are explicit, provisional consensus rules, not proof that an excluded
probe is faulty. They can switch which pair represents the cell, producing
steps. Probe installation depths and soil-specific calibration are needed to
justify a bulk-soil interpretation. If the probes represent different layers,
a depth-weighted observation operator should replace this consensus index.
Do not silently assign A1/A2 to Cells 2/3.

## Reduced model

The model uses the existing **OpenHydroQual System and GA engine**, with custom
reservoir templates. It has six dynamic reservoirs: pond and soil storage in each
of three cells. Detailed gutters, pipes, aggregate storage, native-soil hydraulics
and separate evaporation are removed for this approach test.

For cell i, P is pond storage and S is effective soil storage:

- Rain input = rainfall × (pond area + 0.95 × contributing catchment area).
- Infiltration = k_in × P × max(0, 1 − S / soil capacity).
- Effective drainage = k_drain × S.
- Overflow = 48/day × max(0, P − pond area × overflow depth), routed 1 → 2 → 3 → outlet.
- dP/dt = rain + upstream overflow − infiltration − overflow.
- dS/dt = infiltration − effective drainage.
- Modeled moisture index = 100 × S / soil capacity.

**Exactly two fitted parameters, shared by all three cells:** k_in and k_drain
(1/day). These are effective response rates, **not measurements of Ksat, alpha,
or n**. Effective drainage lumps losses, including omitted evaporation. Pond
area (11.3032032 m²), soil depth (0.9144 m), catchment grouping and nominal
storage range (0.35 × area × depth) come from the existing model and remain fixed.
Overflow depth is a provisional common 0.21 m. Cells 2/3 use the same initial
index as Cell 1 as an explicit simplifying assumption, but have different rain
inputs and receive no observations or copied Cell 1 state updates.

The first observation initializes the storage index; it is from the training
period. OHQ's unevaluated boundary output at the exact initial time is omitted
from exported series. An analytic dry-recession integration test checks the
subsequent computed values. Nominal integration step is 0.001 day, limited to
five times that step, with NR tolerance 1e-7.

Water-level sensor data are **not fitted in this experiment**: MaxBotix's datum
and its well/pond correspondence still need reconciliation, as do HC/weir and
pressure-transducer observation mappings. Adding ambiguous water-level targets
would make this test difficult to interpret.

## Fair validation

- Retrieve the last 14 days of recorded sensors and matching API rainfall.
- Reserve the final three days before any fitting (chronological split).
- Fit only the earlier Cell 1 combined moisture observations: 12 GA candidates,
  8 generations, 2 worker threads. This is a smoke-test optimizer budget.
- Re-run the model from the same initial conditions with fitted parameters frozen
  through the withheld period. There is **no state nudging or validation feedback**.
- Compare against both the identical uncalibrated model and persistence (the last
  training observation held constant).
- Report RMSE, MAE, bias and NSE for training/validation separately.

The holdout is a **conditional hindcast**, because its rainfall is downloaded
for that historical period, rather than using only forecasts archived at the
split date. It is not a verified operational forecasting skill result.
A better fit than an arbitrary untuned model is insufficient; persistence must
also be considered. Do not tune thresholds/parameters on the withheld results
and then claim the same interval as independent validation.

## Reproduce

From the repository root (Qt/OHQ release dependencies must be installed):

```bash
make -C build-qmake -j4
python3 deployments/JM_sensor_experiment/tools/build_runner.py
python3 deployments/JM_sensor_experiment/tools/prepare_data.py \
  --output deployments/JM_sensor_experiment/outputs/my-run

ROOT="$PWD"
RUN="$ROOT/deployments/JM_sensor_experiment/outputs/my-run"
"$ROOT/deployments/JM_sensor_experiment/outputs/experiment" \
  "$ROOT/deployments/JM_sensor_experiment/model/JM.ohq" \
  "$ROOT/resources" "$RUN/input.json" "$RUN"
python3 deployments/JM_sensor_experiment/tools/evaluate.py "$RUN"
python3 -m unittest discover -s tests -p 'test_jm_*.py'
```

The build helper links against the existing release objects; rebuild OHTwin
first after changing engine sources. Data preparation needs network access;
model fitting and evaluation use only the saved inputs. Plotting needs NumPy
and Matplotlib. Large raw data, generated binaries and results are ignored by Git.
GA results can vary slightly between runs; retain the input data, output log and
parameter files with the reported metrics.

## Outputs

- `raw_sensors.json`: unmodified API records for all three probes.
- `observations.json`: accepted/rejected bins, individual values and consensus membership.
- `input.json`, `weather.json`: fixed split, training/validation data and weather provenance.
- `baseline.csv`, `assimilated.csv`: model predictions for all three cells.
- `ga_output.txt`, `detail_GA.txt`, `parameters.json`: calibration results.
- `metrics.json`, `comparison.png`, `RESULTS.md`: validation results and diagnostics.

See the current saved run under `outputs/run-20261117/`. Keep the detailed live
model as the reference until this reduced observation/model combination passes
validation. A negative validation result is a useful outcome, not a reason to
hide the persistence control.

## Follow-up diagnostics

See [FOLLOWUP_RESULTS.md](FOLLOWUP_RESULTS.md) for fixed-membership observation tests and [VALIDATION_PLAN.md](VALIDATION_PLAN.md) for independent validation and safe time acceleration. Reproduce the retrospective comparison with:

```bash
python3 deployments/JM_sensor_experiment/tools/check_observation_sensitivity.py \
  deployments/JM_sensor_experiment/outputs/run-20261117 \
  deployments/JM_sensor_experiment/outputs/my-sensitivity
```

## Raw-voltage replay and state updates

`tools/prepare_raw_replay.py` restricts the experiment to the raw voltage epoch
from 10 November onward, aggregates complete 15-minute bins, and applies explicit
fixed dry/wet ADC endpoints per probe. The upstream raw values are rounded to
0.01 V; they do not recover full ADC precision. A0 and A2 are fitted and evaluated
separately as diagnostic controls. A1 transitions are recorded without assuming
all discontinuities are noise or automatically discarding its live data.

The runner's optional `rolling_state_test` performs a **separate state-assimilation
experiment** after parameter fitting. Frozen parameters are used throughout the
test. Every six hours, Cell 1 soil storage is adjusted by a fixed gain of 0.5
toward the latest observation. A centered bin is available only at its end.
Pond and Cells 2/3 retain their modeled states. Subsequent predictions cannot
use later observations; tests check this explicitly. Analysis water-volume
increments are logged, and corrected storage remains within zero and capacity.
This demonstration is not an EnKF or a Bayesian uncertainty treatment.

`assimilated.csv` remains the frozen-parameter hindcast for backward compatibility.
`rolling_forecasts.csv` contains the separate causal state-updated forecasts.
`tools/evaluate_state_replay.py` compares both with same-origin persistence at
1, 6, 24, 48 and 72 hours where truth is available. Ground truth is interpolated
between adjacent 15-minute medians; gaps over 30 minutes are excluded.

The current saved run is `outputs/raw-state-replay-20261117/`; its RESULTS.md
contains the numbers and limitations. This reuses inspected historical test dates
and historical API rain, so it is retrospective conditional-hindcast evidence,
not independent operational forecast validation. There are no 72-hour scores.

Reproduction, using the saved raw-voltage audit and first-run weather:

```bash
python3 deployments/JM_sensor_experiment/tools/prepare_raw_replay.py \
  deployments/JM_sensor_experiment/outputs/raw-voltage-audit-20261117 \
  deployments/JM_sensor_experiment/outputs/run-20261117 \
  deployments/JM_sensor_experiment/outputs/my-raw-replay
```

Run the existing experiment executable and evaluator on each generated `a0` and
`a2` input directory, then run `tools/evaluate_state_replay.py` on their parent.
No live deployment changes are necessary. See RAW_DATA_AUDIT.md for the correction
to the earlier observation-switch diagnosis and VALIDATION_PLAN.md for promotion.
