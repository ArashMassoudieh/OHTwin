# Session handoff — Wetland codegen assimilation, runs 1–3 (2026-09-26)

Current work stream. The earlier Bioretention ensemble-collapse handoff is
preserved below and is still the reference for that experiment.

## Where things stand in one paragraph

Three state-handling bugs that made the wetland drain to empty are **fixed and
merged** (`657acc0`). Two full two-year runs have completed on this machine and
are archived; they establish that the assimilation recovers most parameters well
but that `Evap_Coefficient` is poorly determined and drags `Soil_Hydraulic_
Conductivity` high with it. **Run 3 is set up to run on the i9** (16 chains / 16
threads) to settle whether that residual is under-sampling or non-identifiability.
Everything below is what you need to check it.

## Important: the twin still runs the INTERPRETER

`OHTwin` has **no codegen kernel wiring** — no `solver_backend`, no
`codegen_library`, no `KernelSystem` anywhere in the sources, and the built
binary contains zero `ohq_kernel` symbols. The `_codegen` suffix names the
deployment copy, not the solver. Phase 2 of
`OpenHydroQual/../OpenHydroTwin/CODEGEN_ROADMAP.md` is unwritten. A kernel would
cut the 3.6 s forward solve to ~0.04 s, which is the real fix for the sampling
budget — but it is not in play today.

## The three runs

| | run 1 | run 2 | run 3 (i9) |
|---|---|---|---|
| deployments | `Wetland_*_codegen` | `Wetland_*_codegen_mt` | `Wetland_*_codegen_i9` |
| ports truth / assim | 8090 / 8186 | 8091 / 8187 | **8092 / 8188** |
| chains | 8 | 8 | **16** |
| MCMC `number_of_threads` | 1 | 8 | **16** |
| solver `n_threads` | 8 | 1 | 1 |
| `mcmc_max_sweeps` | 300 | 300 | **900** |
| dates | 09-16 → 09-20 | 09-22 → 09-26 | pending |

Model physics is byte-identical across all three — the only model diffs are
`number_of_chains` and `number_of_threads` (lines 14 and 17 of `Wetland.ohq`).
`time_acceleration` is 200 throughout, so a cycle's budget is
`3 × 86400 / 200 − 60 = 1236 s` in every run.

## Results so far

Last-50-cycle means against known truth:

| parameter | truth | run 1 | run 2 |
|---|---|---|---|
| CatchmentRunoffCoeff | 0.75 | +3% | **+1%** |
| PondAlphaMultiplier | 1.0 | +16% | **+1%** |
| WetlandOutletAlpha | 100000 | +14% | **−2%** |
| Evap_Coefficient | 1.0 | −98% | −43% |
| Soil_Hydraulic_Conductivity | 0.001004 | +668% | +276% |
| Stage_Std / injected 0.005 | | 1.6× | 1.2× |
| Outflow_Std / injected 20 | | 2.0× | 1.5× |

Sampling: run 1 gave 42 sweeps and 344 evals per cycle, ESS median 10, 18/241
cycles converged. Run 2 gave **288 sweeps, 2312 evals, ESS 29, 104/244
converged** — a 6.7× throughput gain at zero wall-clock cost, purely from
`number_of_threads` 1 → 8.

**The open question.** ET no longer collapses but *wanders*: second-half median
0.56, range 0.15–1.31, in a run where 43% of cycles certify. Its neighbours pin
to 1–2%. That asymmetry suggests `Evap_Coefficient` is weakly identified rather
than under-sampled — it binds to **both** `solar_scale_fact` and
`wind_scale_fact` on the Penman source, with `Stage_Std` free to absorb the
residual. The ksat over-estimate tracks it (6.8× → 3.8× median over the second
half), consistent with the model replacing a missing ET sink with seepage.

Run 3 tests the sampling side. If ET still wanders at ~2× the evaluations, treat
it as identifiability and the next experiment is splitting the two scale factors
into separate parameters. Only then does tightening the ET prior become a
modelling statement rather than a patch.

## Checking run 3 on the i9

Full build/run instructions: `deployments/RUN_i9.md`. Folder layout on the i9 is
identical to this machine, so the nginx block needs no path edits.

Confirm on the first calibration cycle (~21 min in):

```
runCycle: cycle=1 chains=16 threads=16
[Assim] weather injected over ... : precip N bins, T ..., R_h ..., wind ..., rad ...
```

`threads=1` means the model edit did not take. Any zero count in the weather
line now aborts the cycle loudly instead of silently solving an unforced window.

Progress at a glance:

```bash
D=deployments/Wetland_assimilation_codegen_i9
tail -3 $D/outputs/calibration/parameter_history.csv
python3 -c "import json;r=[json.loads(l) for l in open('$D/outputs/calibration/posterior_history.jsonl')];print('converged %d/%d'%(sum(1 for d in r if d.get('converged')),len(r)))"
grep -E "spin-up window|weather injected" $D/outputs/debug.log | tail -3
```

(`posterior_history.jsonl` is written compact, so a `grep '"converged": true'`
with a space silently returns 0 — use the python form, or
`grep -c '"converged":true'`.)

Expect ~3.8 days wall-clock regardless of CPU — the QTimer paces at 432 s per
simulated day, so a faster box samples more per cycle, it does not finish sooner.

Figures, with every caption computed from the run being plotted:

```bash
python3 deployments/Plots/analyse_wetland_codegen_run1.py \
        --suffix _i9 --label "16 chains / 16 threads" \
        --outdir deployments/Plots/wetland_run3
```

## What was fixed (merged, `657acc0`)

1. **`buildSpinupSnapshot` solved the wrong window.** `SetProp()` followed by
   `SetSystemSettings()` — which re-pushes every stored Settings value — silently
   reverted the override, so the spin-up solved the script's **2009** window
   (40178.8 → 40542.8) while forcing was injected for **2020**. Every rain
   interpolation fell outside the series, the spin-up ran 364 unforced days and
   drained the model to exactly 0. Now writes the window into the
   `General Settings` object first (as `prepareCalibrationSystem` already did)
   and verifies it stuck. This only bit from cycle ~20, when the record first
   exceeds `calibration_window_days` and the rolling branch activates — which is
   the clue that located it.
2. **`DTRunner` adopted the calibrated snapshot wholesale.** Its comment says the
   snapshot is taken for its *parameters*, but the code copied the whole System
   JSON including block storages — and `DTAssimilation` writes that snapshot
   deliberately unsolved. State now comes from the latest forward snapshot; only
   `Parameters` / `Set As Parameters` are merged in (`Solve(true)` applies them).
3. **Weather injection could not report failure** (`Q_UNUSED(errorMessage)`,
   returned `true` unconditionally, and the spin-up discarded the result). Now
   fails on an empty precipitation fetch and logs per-variable sample counts.

Result: **0 of 17,689** output points below 0.01 m over two years, versus 1437 of
4369 before; `Stage_Std` 29× → 1.6× the injected noise.

## Archives — results live OUTSIDE the repo

`deployments/*/{outputs,state,snapshots}/` are gitignored (a two-year run is
~100 MB). Completed runs are archived whole:

```
~/Projects/OHTwin_results_archive/
  20260920_wetland_codegen_run1.tar.gz        19 MB  + .README.md
  20260926_wetland_codegen_run2_mt.tar.gz     23 MB
  wetland_run3_i9_deployments.tar.gz          27 KB  (transfer bundle)
```

The run-1 README records settings, results and open issues in the same shape as
this section. Archive run 3 the same way when it finishes.

## Gotchas specific to this work

- **Run every OHQ tool from the model's folder** — `addtemplate` resolves names
  against the process CWD before `resources/` (`OpenHydroQual/issues.md` ISSUE 6).
- Both twins need `--fresh`; resume anchors sit past `stop_datetime`.
- Truth first, then the assimilator, and only a little ahead — the advance has no
  upper clamp, so a finished truth triggers one enormous catch-up solve.
- `number_of_threads` (MCMC, spreads chains over cores) and `n_threads` (solver)
  are different settings. The solver one must be **1**: samples already run in
  parallel, and `posteriorLocal` forces `SetNumThreads(1)` per sample anyway.
  Threads beyond the chain count do nothing.
- `MCMC/number_of_threads` defaults to **1** in `resources/settings.json`. That
  single line cost run 1 a 6.7× throughput factor.
- nginx sites for the *older* deployments still alias
  `/home/arash/Projects/DrywellDT/...`, which no longer exists — port 8088
  returns 404. Only the `wetland_codegen*` blocks are correct.
- The Qt Creator viewer deploys `viewer/config.json` next to the binary on every
  build; repoint its URLs when switching runs.

## Key files for this stream

- `DTRunner.cpp` (~line 600, snapshot adoption), `DTAssimilation.cpp`
  (`buildSpinupSnapshot`, `injectCalibrationWeather`, `prepareCalibrationSystem`)
- `deployments/RUN_i9.md` — build + run instructions for the i9
- `deployments/Plots/analyse_wetland_codegen_run1.py` — the four figures,
  `--suffix` selects the run
- `deployments/wetland_codegen{,_mt,_i9}.nginx` — server blocks per run
- `OpenHydroQual/issues.md`, `OpenHydroTwin/CODEGEN_ROADMAP.md`

---

# Earlier handoff — ensemble collapse diagnosis + anti-collapse work (2026-08-06)

*Superseded as the current state by the section above, but still the reference
for the Bioretention drift experiments and the anti-collapse machinery.*

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
