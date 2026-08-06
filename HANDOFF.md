# Session handoff — ensemble collapse diagnosis + anti-collapse work (2026-08-06)

Supersedes the 2026-07-27 handoff. Binary built and clean: `build-qmake/bin/OHTwin`.

## Where things stand in one paragraph

The proposal-kernel fixes from the previous session **worked mechanically and
failed scientifically**. Acceptance went 0.0086 → 0.166, ESS 24 → 73, convergence
0/244 → 172/244 — and inference got *worse*: 95% CI coverage of the known truth
fell to 10% (3% at accel 100), and Ksat ended at 6.17 against a truth of 2.0,
never recovering. Cause: **ensemble variance collapse**. A test run
(`Bioretention_assimilation_MCMC_drift_INFLATE`) is now in flight testing the
first countermeasure.

## The three completed runs

| deployment | config | outcome |
|---|---|---|
| `..._MCMC_drift_BASELINE` | frozen proposal (pre-fix) | 0/244 converged, but **accurate**: Ksat 2.20 vs truth 2.0, 93-day recovery lag |
| `..._MCMC_drift` | fixed kernel, accel 200 | 172/244 converged, coverage **10%**, Ksat 6.17 (+209%), never recovers |
| `..._MCMC_drift (Copy)` | fixed kernel, accel **100** | 186/244 converged, coverage **3%**, Ksat 7.56 (+278%) |

Truth outputs for run 1 are preserved at `Bioretention_truth_drift_RUN1_outputs/`.

**The accel-100 run is the key evidence:** doubling the compute per cycle made
every scientific measure worse while improving every diagnostic. That rules out
undersampling and identifies a self-reinforcing contraction driven by sampling
steps.

## Diagnosis (all measured, not inferred)

- Pool spread collapsed **377×** (0.147 → 0.00039 over 244 cycles); 81/242 cycles
  had pool sd < 1e-3; cycle 111 pooled **1 distinct draw out of 3**.
- Accumulated Σ̂ moved only **1.7×**, so it ended **250–1000× wider** than the
  posterior it preconditions. It isn't collapsing — it's *frozen* (no forgetting,
  W reached ~60,000, each cycle shifts it ~1%).
- κ absorbed the mismatch until it **saturated at its 1e-4 floor**.
- Contraction locus: **warm-start seeding**. Chains draw from the previous
  cycle's already-narrow pool, so under-dispersion compounds every cycle. Extra
  sweeps don't escape it — they estimate the collapsed state more precisely,
  which is why CIs got 6.6× tighter at accel 100.
- Measured contraction rate: **×0.976 per cycle**.

Secondary (≈2.45×, i.e. ~150× less important than the collapse): the likelihood
treats 6-hour-correlated noise as independent at hourly sampling.

## What was implemented (all default OFF — nothing changes unless enabled)

### `DTStreamingMCMC` — anti-collapse
- **Seed inflation** `inflateSeedEnsemble()`: `φ ← φ̄ + r(φ − φ̄)` in proposal
  space (log coords for log-normal, so it's geometric in the physical parameter
  and can't go negative). Mean preserved to 4e-16, spread scaled by exactly `r`.
  This is EnKF covariance inflation; chains stay at the mode, so nothing has to
  traverse the prior. Config `mcmc_seed_inflation` (1.0 = off).
- **Configurable proposal-scale floor** `proposalScaleFloor()`, replacing the
  hard-coded 1e-4. Can floor the *effective step* `κ·sd_i` against prior width.
  Config `mcmc_kappa_min`, `mcmc_min_step_fraction` (0 = off).

### `DTStreamingMCMC` — drift detection
- **CUSUM** per parameter on pooled cycle means, in proposal space. Reference
  built over the first `mcmc_drift_reference_cycles` (40), no alarm during that
  period. Runs on provisional cycles too — a drifting system is exactly when
  cycles stop certifying. Online trigger; **no p-value**.
- **Hotelling T²**, two-window, over the full parameter vector → one p-value for
  "has anything drifted". Uses **effective** counts `W/τ` with τ estimated from
  the cycle-mean autocorrelation. F-tail validated against `scipy.stats.f.sf` to
  ~1e-13. Degrades to the diagonal form rather than inverting a rank-deficient
  covariance.
- State round-trips through `posterior_latest.json` under a `drift` key.
  `chooseSeedMode` now gets the real flag instead of hard-coded `false`.
- Config `mcmc_drift_detection`, `mcmc_cusum_k` (1.0), `mcmc_cusum_h` (5.0),
  `mcmc_t2_window_cycles` (33).

Validated offline on BASELINE: CUSUM `k=1, h=5` detects **19 days** after onset
with **0 false alarms** in 110 pre-drift cycles — 74 days before the estimate
itself converges. The two-window T² needs ~100 days but gives p = 4.7e-8.

### `DTAssimilation` — likelihood autoscaling (Layer 1)
- `updateLikelihoodScales()`: one solve per cycle at the point estimate,
  per-observation residual τ_int via Geyer initial-positive-sequence,
  EWMA-smoothed, applied via `Observation::SetLikelihoodScale()`.
- Deliberately **measured, not derived from the noise config**: residual
  correlation is dominated by *model structural error*. Measured τ_int was
  3.6 / 23.8 / 9.5 (underdrain / soil moisture / pond depth) against a
  theoretical 12.0 from the injected noise. A single assumed scale is indefensible.
- C++ estimator validated against a Python reference to 1e-6.
- Config `mcmc_likelihood_autoscale` (false), `mcmc_likelihood_scale_ewma` (0.3).

### Diagnostics added to `posterior_history.jsonl`
`kappa_floor`, `seed_inflation`, `drift_detected`, `cusum_max`, `drift_t2_p`,
`drift_tau`, `drift_parameters`.

## ⚠ OpenHydroQual changes (separate repo, affects every project using the engine)

| change | status |
|---|---|
| Missing ½ in the Gaussian NLL (`fit_mse/(2σ²)`, 3 sites) | **committed** (in HEAD) |
| `likelihood_scale` member + divisor + **copy-ctor/`operator=` propagation** | **UNCOMMITTED on master** |

The copy propagation is not optional: every MCMC chain works on a `System` copy,
so without it the chains would silently run at scale 1.0. `likelihood_scale` is a
plain member, not a `Quan` — it's a property of the residual series, not the
model file, so no template changes and no `.ohq` edits.

DrywellDT has 6 modified files, also uncommitted.

## The run in flight

`deployments/Bioretention_assimilation_MCMC_drift_INFLATE` — see its `RUN.md`.
Port 8185, accel **200** (deliberately not 100), `mcmc_seed_inflation: 1.05`,
drift detection on, **autoscaling off** so inflation is the single intervention.
Everything else identical to `..._MCMC_drift`, which is therefore a clean control.

`r = 1.05` is the smallest value that reverses the measured ×0.976/cycle
contraction (net ×1.025).

### Checking it

```bash
python3 deployments/Bioretention_assimilation_MCMC_drift_INFLATE/check.py
```

| when | what it should show |
|---|---|
| cycle 2+ | `seed inflation: r=1.05 applied to 16 chains` in `debug.log` |
| **cycle 40–60** | **pool spread stabilises** instead of falling — the test. Control was down ~100× by cycle 40 |
| cycle 40–60 | Σ̂/pool ratio near 1–5, not 200–1000; κ off its floor |
| cycle 66+ | detector armed: `drift_t2_p` > 0.05, `cusum_max` < 5 pre-drift |
| cycle 111–121 | drift; compare CUSUM lag against baseline's +19 d |
| end | **coverage** — control 10%, nominal 95% |

## Next steps, in order

1. **Read the verdict at cycle 40–60.** If pool spread is still falling
   geometrically, r=1.05 is too weak → retry at 1.10. If it holds, proceed.
2. **If spread holds but coverage stays under ~20%**, enable
   `mcmc_likelihood_autoscale: true` (already implemented) and re-run. Expect
   ~2.45× widening — helpful but not sufficient alone, by construction.
3. **If κ is still pinned at its floor** despite the spread holding, enable
   `mcmc_min_step_fraction` (~1e-3).
4. **Commit the OpenHydroQual `likelihood_scale` change** — ideally on a branch,
   since it touches the shared engine and the GA path uses the same objective.
5. **Do not implement `seedRatioWeighted`.** It resamples (concentrates) exactly
   when dispersion is needed, has nothing to work with on a collapsed pool, and
   wouldn't fix the lag anyway — the 93-day recovery is set by
   `calibration_window_days`, not by seeding. The cheaper drift response is
   temporary window shortening plus an inflation boost, both existing knobs.

## Scope note for the paper

Agreed this is at the complexity limit. Suggested framing:
- The ½ fix and the Σ̂/κ persistence are **bug fixes** — a footnote, not method.
- Seed inflation is **EnKF covariance inflation** (Anderson & Anderson 1999;
  Anderson 2007) — one paragraph, one equation, one citation.
- Likelihood autoscaling is the **effective-sample-size / design-effect**
  correction — one paragraph, and it replaces defending an arbitrary constant.
- Drift detection (CUSUM + T²) is a **separate contribution**; bolting it on will
  dilute both.
- Every knob defaults to off, so **describe only what was enabled in the reported
  runs**. If inflation alone fixes the collapse, autoscaling never appears.

There is also a genuine finding here worth stating plainly: *adaptive
preconditioning improved every standard sampler diagnostic — acceptance, ESS,
convergence rate — while degrading inference accuracy and destroying interval
coverage.* The baseline is the controlled comparison.

## Gotchas

- Both twins need `--fresh`; resume anchors sit past `stop_datetime` so they
  otherwise quit immediately having done nothing.
- Truth first, then the assimilator — `advanceEnd = tMaxDt` has **no clamp**, so
  starting the assimilator against a finished truth attempts one ~730-day solve.
- Truth and assimilator must share `time_acceleration` or the frontier runs away.
- One truth + one assimilator per deployment directory.
- `logging.truncate: true` wipes `debug.log` on start — copy it before restarts.
- Port 8185 isn't nginx-served (no viewer). The run doesn't need it.

## Key files

- `DTStreamingMCMC.{h,cpp}`, `DTAssimilation.{h,cpp}`, `DTConfig.{h,cpp}`
- `OpenHydroQual/aquifolium/{include/observation.h,src/observation.cpp}` (uncommitted)
- `deployments/Bioretention_assimilation_MCMC_drift_INFLATE/{RUN.md,check.py}`
- `streaming_mcmc_algorithm.tex`, `proposal_adaptation_methods.tex` (both compile;
  **not yet updated** with the collapse findings or the new mechanisms)
