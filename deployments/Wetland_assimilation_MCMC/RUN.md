# Wetland streaming-MCMC drift run — instructions

Two processes: a **truth twin** that generates synthetic sensor data with a
drifting parameter, and an **assimilation twin** that must track that drift
without being told. They communicate over HTTP through nginx, not directly.

Binary: `/home/arash/Projects/DrywellDT/build-qmake/bin/OHTwin`
(rebuild with `cd /home/arash/Projects/DrywellDT/build-qmake && make -j$(nproc)`)

## The experiment

The truth drives **`Soil_Hydraulic_Conductivity`** from 0.01995 → 0.00100 m/day
(a ~20× smooth decay, i.e. progressive clogging) across the full two-year record,
via `Wetland_truth/drift/ksat_drift_wetland.csv` (732 daily points). This is a
*gradual* drift, unlike the bioretention experiment's abrupt step.

The assimilator calibrates 7 parameters — `CatchmentRunoffCoeff`,
`WetlandOutletAlpha`, `PondAlphaMultiplier`, `Evap_Coefficient`,
`Soil_Hydraulic_Conductivity`, `Stage_Std`, `Outflow_Std` — against three
observations, all Weighted Least Squared with kernel Δ₀ = 30 d and a calibrated σ:

| observation | σ parameter | truth noise |
|---|---|---|
| Wetland outflow (m³/day) | `Outflow_Std` | 0.1 (10%, multiplicative) |
| Wetland inlet stage (m) | `Stage_Std` | 0.1 |
| Wetland outlet stage (m) | `Stage_Std` | 0.1 |

## Order matters

nginx serves port 8088 from `Wetland_truth/outputs/`, and it currently holds a
**complete** run from a previous session (last t = 44565). `DTRunner` advances to
the observation frontier with **no upper clamp**, so starting the assimilator
against a finished truth makes it attempt a single ~730-day solve.

**Start the truth first, let it get a few cycles ahead, then start the assimilator.**

### 1. Truth

```bash
cd /home/arash/Projects/DrywellDT && ./build-qmake/bin/OHTwin --deployment deployments/Wetland_truth --fresh --force
```

Wait until `outputs/selected_output.csv` exists and is growing (~30 s), then:

### 2. Assimilator

```bash
cd /home/arash/Projects/DrywellDT && ./build-qmake/bin/OHTwin --deployment deployments/Wetland_assimilation_MCMC --fresh --force
```

`--fresh` erases `state/` **and** `outputs/` recursively, including
`outputs/calibration/`, so the MCMC cold-starts instead of warm-starting from a
stale posterior. `--force` skips the confirmation prompt — drop it to be asked.

Both **require** `--fresh`: their resume anchors (`_dt_next_start_utc`) are past
`stop_datetime`, so without it they quit immediately having done nothing.

> `Wetland_assimilation_MCMC/outputs/` and `state/` currently hold a **stale copy
> of bioretention results** placed there by mistake. `--fresh` clears them. The
> authoritative bioretention results are in
> `deployments/Bioretention_assimilation_MCMC_drift{,_BASELINE}`.

## Expected duration

Both run at `time_acceleration: 200` over 2020-01-03 → 2022-01-03 (728 days).
The assimilator calibrates every `poll_interval` = 3 simulated days = **21.6 min
wall clock**, so ≈ **244 cycles ≈ 88 hours**. The truth advances 3 simulated days
per assimilation cycle, keeping the two in lockstep — this is why both are at 200.

## Check within the first ~10 cycles (~4 h) before letting it run to completion

```bash
grep -E "proposal covariance:|adapt block:|cycle .* published" deployments/Wetland_assimilation_MCMC/outputs/debug.log | head -40
```

Healthy signs:

- `proposal covariance: READY -- preconditioning proposals` from **cycle 2**
  (cycle 1 runs the legacy walk — no Σ̂ accumulated yet)
- `kappa=` near 0.79 and moving, **not** pinned at 1e-4
- acceptance rising off ~0.01 toward the 0.15 target
- `plateaued_fraction` becoming non-zero (it was 0 in every cycle before the fix)
- forward `sim_days` in `outputs/run_log.csv` staying at 3.0 — a large value means
  the truth stopped and the frontier jump triggered

If `produceRealizationCI: BAIL reservoir empty` dominates, or acceptance stays at
~1%, stop and report — the proposal kernel is not engaging and the remaining 84
hours would be wasted.

## Harmless messages

These appear in both twins and are **not** errors to act on:

- `File main_components.json was not found!` (×6) — the loader tries the path as
  given, fails, then succeeds via the default template directory. Verified: a run
  with absolute template paths produces bit-identical output on every
  noise-free series.
- `Error: property 'Evapotranspiration' does not exist in 'Receiving Water'`
  (×8, similar) — pre-existing quirks of the wetland model definition.

## Notes

- Run exactly **one** truth and **one** assimilator. Duplicates race and
  double-write history.
- Port 8184 (the assimilator) is **not** nginx-served. The run doesn't need it;
  only the browser viewer would. Ask Arash if you want the viewer wired up.
- Outputs to keep for analysis: `outputs/calibration/` (all of it),
  `outputs/posterior_dist_*`, `proposal_cov_cycle_*`, `chain_traces_cycle_*`,
  `outputs/run_log.csv`, `outputs/debug.log`, and the per-cycle
  `outputs/*_forecast_output.txt` archive (`keep_debug_outputs: true`).
- `logging.truncate: true` wipes `debug.log` at startup, so copy it before any restart.
