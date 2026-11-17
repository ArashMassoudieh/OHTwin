> Update: see RAW_DATA_AUDIT.md. Raw A1 voltage has substantial jumps; the earlier membership-switch explanation alone is incomplete.

# JM observation and model follow-up

## Time acceleration

Historical experiments run locally as fast as the OHQ solver allows. Physical
model time, rainfall times and sensor times remain the original UTC dates.
The 14-day replay does not require waiting 14 days.

The full twin also supports `runtime.time_acceleration`: it changes timer cadence,
not timestamps. For a future full-runner replay, use a separate deployment with
explicit historical start/stop dates, archived forcing and observations, separate
outputs/state/snapshots, and no live upload targets. Do not accelerate a live
station and reuse its future-dated snapshot after resetting acceleration to 1.
Return to the live deployment's existing real-time configuration and state.
No live timing configuration was changed for these experiments.

## What the first follow-up tests

Compare the original changing-consensus observation with:

- A fixed equal-weight average of A0, A1 and A2. Require every probe in every
  accepted 15-minute bin; never change weights or substitute a different pair.
- A1 alone, as a diagnostic control. This is not evidence A1 is the correct
  representative probe, or that the other probes are defective.

Keep the model, parameter bounds, GA budget, rainfall and chronological split
unchanged. Fit each observation definition independently on its training data.
Compare every model with persistence for the same definition and exact scored
samples. Errors across definitions are not directly equivalent because targets
and missing-data coverage differ.

These are retrospective sensitivity tests on data already inspected, including
the original holdout. They cannot establish independent predictive skill.
Fixed membership removes pair-switching artifacts but does not calibrate the
probes or establish equal physical representativeness.

## Next independent test

Freeze a physically justified observation definition after confirming probe
positions/depths and calibration. Do not infer these from channel names.
Keep the existing provisional index distinct from volumetric water content.
The current model assumes the index is proportional to drainable storage;
that assumption is not established by dry/wet endpoint calibration.

Before retrieving the next evaluation period, record the cutoff, observation
rule, training window, forecast horizon and metrics. Use only training data for
parameter fitting and initial-state estimation. Evaluate 1-, 6-, 24- and 72-hour
forecasts against persistence on the same available timestamps. Report both a
dry interval and an independent rainfall event; do not select successful dates.
Historical weather supports a conditional hindcast only. Operational forecasting
requires the weather forecast available at each forecast origin.

A state-assimilation experiment must also explicitly update model storage from
past observations, preserve bounds and account for the introduced water-volume
increment. The experiments to date perform parameter calibration only; they do
not demonstrate a streaming state-assimilation method.

## Promotion gate

Do not replace either live model yet. First require improvement against persistence
across the declared horizons and events, realistic storage/ponding behavior, no
future observation leakage, and an explained observation-to-storage mapping.
Keep Cells 2 and 3 model-only until they have their own observations. Use shared
parameters as a stated assumption, not evidence of validation in those cells.

## Implemented state test

The raw-state replay now implements the separate causal fixed-gain soil-storage
update described above. Water-volume corrections and causality are explicitly
tested. It remains retrospective; a new independent event and a justified
observation-to-storage mapping are still required before promotion.
