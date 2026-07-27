# Session handoff — proposal-kernel fixes + paper instrumentation (2026-07-27)

Work on `DTStreamingMCMC` / `DTAssimilation` and the OpenHydroQual likelihood,
preparing a re-run of the bioretention **drift** experiment for the methods paper.

Previous handoff (2026-07-06→07: rolling window, MAP spin-up, seamless resume)
is superseded by this file; its open item #2 ("convergence never certifies") is
diagnosed and addressed below.

## Starting point

`deployments/Bioretention_assimilation_MCMC_drift` held a finished 244-cycle run
(t_now 43837→44565) against `Bioretention_truth_drift`, in which the truth drives
`EngSoilKsat` 10 → 2 across t=44166–44197 via `drift/ksat_drift.csv`.

**Scientific result was good.** Ksat tracked the step with a **99-day recovery
lag ≈ `calibration_window_days`=100**; `EngineeredSoilAlpha/Ksat/n` and
`RunoffCoeff` recovered to within a few percent of truth; CI width inflated ~3×
during the shock and re-tightened. `NativeSoilKsat`/`NativeSoiln` are not
identifiable from the three calibration observations (relative CI width
250–650%, estimates outside their declared prior bounds), which shows up
downstream as a persistent −0.45 m³/day groundwater-recharge bias.

**Sampler health was not.** `converged=false` and `plateaued_fraction` **exactly
0 in all 244 cycles**; acceptance median 0.0086 against a target of 0.15.

## Root cause (three interacting defects, all silent)

1. **Σ̂ did not survive the cycle.** `DTStreamingMCMC` is a stack local rebuilt
   every cycle (`DTAssimilation.cpp:1090`). `accumulateProposalCovariance` runs
   *after* the sampling loop "so the NEXT cycle benefits", but the object is
   destroyed immediately and `m_propCov` was not persisted. `m_propReady` was
   therefore true for **zero proposals ever drawn**.
2. **κ pinned at its floor.** `m_kappa` initialised to 0.0 with its lazy init
   inside `refactorProposalCholesky()` — reached only at cycle end. Every
   adaptation block computed `0 × 0.75 = 0` and the clamp lifted it to 1e-4,
   ~8000× below κ₀ = 2.38/√d.
3. Consequently `mcmc_proposal_mode: "covariance"` degraded to the **legacy walk
   with adaptation switched off**: `adaptProposalScale` takes the κ branch and
   returns before touching `pertcoeff`. Verified — all nine final `pertcoeff`
   match `0.05 × range` to machine precision (rel. diff ≤ 2.4e-16).

Chain: frozen proposal → ~1% acceptance → below the classifier's motion guard
(`minAcceptedFraction × plateauWindow` = 0.05 × 60 = **3 accepts per 60 sweeps**,
i.e. ≥5% acceptance) → chains thrash PLATEAUED↔REVERTED (3145 vs 3179 events) →
`plateauedFraction()` = 0 at cycle end → quorum never fires.

**Lowering `mcmc_quorum_fraction` cannot help — its input is already 0.**
Switching to `"global"` is also wrong: that is the mode whose failure
`proposal_adaptation_methods.tex` §1 documents (global scale collapses to
~1.5e-4 while the weakly-identified directions freeze).

## Changes

### `DTStreamingMCMC.{h,cpp}` — kernel fixes
- Σ̂ / `W` / κ round-trip via `posterior_latest.json` (`proposal_covariance`,
  `proposal_covariance_weight`, `kappa`). Restored **and refactorised** in
  `initializeCycle`, so `m_propReady` is armed from sweep 1. Written on
  provisional cycles too — the proposal shape is a sampler property, not a
  posterior claim, and nothing certifies.
- `m_kappa = 2.38/√d` in `initializeCycle`, before any adaptation block.
- Adapt-block log now prints `kappa=` and `cov=ready|not-ready`. It previously
  printed only `pertcoeff[0]`, the one value guaranteed not to move in this mode.

The accumulation math was already what we want and is **unchanged**: weight
`n_k−1` with per-cycle mean-centering expands to the pooled within-cycle
covariance `Σ_k S_k / Σ_k (n_k−1)` — every retained draw equally weighted, drift
removed. Duplicates (rejected steps re-pushing state) are kept; they are part of
the correct MH estimator.

### `OpenHydroQual/aquifolium/src/observation.cpp` — likelihood
`fit_mse/σ²` → `fit_mse/(2σ²)` at lines 126, 137, 199 (LS normal, LS log-normal,
WLS). `diff2`/`weighted_mse` both return *means*, so the `N`/`sum_w` multiplier
was already correct; only the ½ was missing. Verified numerically: old argmin =
√2 × RMS, new = RMS, and the new expression differs from the textbook Gaussian
NLL by exactly ½N·log(2π).

**This does not change θ inference.** Profiling σ out gives
`const + (N/2)·ln MSE(θ)` under both forms — it corrects σ only. Every
`Std_*` from runs predating the fix is inflated by exactly √2. Separate repo,
uncommitted on `master`; affects every model using the engine, including the GA path.

### Paper instrumentation (write-only; cannot alter run behaviour)
- `posterior_history.jsonl` += `kappa`, `prop_cov_ready`, `prop_cov_weight`,
  `pertcoeff`, `seed_mode`, `unique_pool_size`, `chain_logp_{min,median,max}`.
- `proposal_cov_cycle_<t>.txt` — Σ̂, marginal SDs, derived correlation matrix.
- `chain_traces_cycle_<t>.csv` — `chain,sweep,logp,accepted,phase`. Needed a new
  uncapped mirror of `chain.trace` in `DTChainState`, since the classifier's
  deque is capped at `plateauWindow`=60 and destroys the climb→plateau path.
- Both gated on `mcmc_realization_interval`.

### Configs
- `Bioretention_truth_drift`: `time_acceleration` **400 → 200**. The baseline was
  produced at 200 — at 1296 s wall per assimilation cycle, a truth at 200
  advances exactly the observed 3.0 sim-days/cycle (400 gives 6.0, halving the
  cycle count and the resolution of every trajectory figure).
- `Bioretention_assimilation_MCMC_drift`: `keep_debug_outputs` **false → true**.
  This is the *only* thing that archives per-cycle forecasts, as
  `outputs/<yyyyMMdd_HHmmss>_forecast_output.txt` named by forecast issue time.
  `mcmc_realization_interval` left at 10 by request.

### Docs
`streaming_mcmc_algorithm.tex` (likelihood ½, covariance mode, κ, analysis
dumps, measured certification behaviour, stability-path defect) and
`proposal_adaptation_methods.tex` (Method C persistence + κ init; the
"persists across cycles" claim was wrong). Both compile clean — 14 and 7 pages,
no undefined refs.

## Build

Clean. `build-qmake/bin/OHTwin`. Only warnings are pre-existing (sign-compare in
`posteriorLocal`, QDateTime deprecations, unused `mark` in DTAssimilation).

```bash
cd build-qmake && make -j$(nproc)
```

## Running

**Order matters.** nginx serves 8086 from `Bioretention_truth_drift` and
currently returns the *complete stale* CSV (3.2 MB, last t=44565).
`advanceEnd = tMaxDt` at `DTRunner.cpp:522` has **no clamp**, so starting the
assimilator against a finished truth attempts a single ~730-day solve.

Truth first, then the assimilator:

```bash
cd /home/arash/Projects/DrywellDT
./build-qmake/bin/OHTwin --deployment deployments/Bioretention_truth_drift --fresh --force
```

```bash
cd /home/arash/Projects/DrywellDT
./build-qmake/bin/OHTwin --deployment deployments/Bioretention_assimilation_MCMC_drift --fresh --force
```

`--fresh` erases `state/` **and** `outputs/` recursively (including
`outputs/calibration/`), so the MCMC cold-starts rather than warm-starting from
the stale end-of-run posterior. `--force` skips the confirmation prompt; drop it
to be asked. Without `--fresh` both quit immediately — their resume anchors
(`_dt_next_start_utc` = 2022-01-04) are past `stop_datetime` = 2022-01-03.

Expect ~244 cycles at ~21.6 min each ≈ 88 h.

## Verify in the first ~10 cycles (~4 h) before committing to the full run

```bash
grep -E "proposal covariance:|adapt block:|cycle .* published" \
  deployments/Bioretention_assimilation_MCMC_drift/outputs/debug.log | head -40
```

- `proposal covariance: READY` from **cycle 2** (cycle 1 is legacy — no Σ̂ yet;
  one cycle's pool gives W≈25 > 2d=18)
- `kappa=` moving off 0.793, **not** pinned at 1e-4
- acceptance climbing off 0.01 toward 0.15
- `plateaued_fraction` becoming non-zero
- forward `sim_days` staying at 3.0 in `run_log.csv` — a large value means the
  truth is not keeping pace and the frontier jump has triggered

If `produceRealizationCI: BAIL reservoir empty` still dominates, stop early.

## Backup

`deployments/Bioretention_assimilation_MCMC_drift_BASELINE` is a verified
byte-identical copy of the 244-cycle results (`diff -rq` clean, all key files
md5-match). It is **writable and runnable** with the same port and name — worth
`chmod -R a-w`. `Bioretention_assimilation_MCMC_drift (2)` is an empty stub, not
a backup. Same-filesystem, so no protection against disk loss.

## Open items

1. **The ablation is confounded.** BASELINE has both the frozen proposal and the
   old likelihood. Attributing acceptance/ESS gains to the proposal fix is
   defensible (the likelihood fix is profile-equivalent in θ), but σ values are
   **not** comparable across the two. A clean isolation needs a third run with
   fixed likelihood + `"global"`.
2. **Stability path (Path 2) still cannot fire.** `m_pointEstimateHistory` is
   cycle-local and gets one push per cycle, so `streamStable()` always fails its
   `size() >= stabilityWindow` (5) test. Quorum is the sole criterion regardless
   of `mcmc_stability_enabled`. Fix is the same shape as Σ̂: persist the ring.
3. **Accumulation weighting.** `n_k−1` over-credits thin cycles — pools average
   26.6 draws but only 15.2 distinct. ESS weighting was considered and
   deliberately not adopted (equal per-sample weighting was requested).
   The `n < d+2` guard also tests raw pool size, not distinct count.
4. **Recovery-lag vs window-length figure** needs multiple runs at different
   `calibration_window_days` — a study-design decision, not instrumentation.
5. Four deployments declare port 8086 (`Bioretention_truth_drift`,
   `..._soil_std=0.01`, `Bioretention_sensitivity_analysis`,
   `R_sensitivity_analysis`). Only the first has outputs, so it is unambiguous
   today, but starting two would silently cross-wire the truth feed.

## Gotchas

- One MCMC + one truth instance per deployment directory. Duplicates race and
  double-write history.
- `t_now` in calibration outputs = `m_buffer.tMax()` — advances only when the
  truth publishes.
- `posterior_samples.csv` is the *only* cumulative record of individual draws
  (one row per pooled draw, every cycle) and is what makes windowed posterior
  densities possible given ~26-draw cycles. Pool only within one regime — each
  cycle targets its own sliding-window posterior.
- `logging.truncate: true` wipes `debug.log` at startup.

## Key files

- `DTStreamingMCMC.{h,cpp}`, `DTAssimilation.cpp`, `DTRunner.cpp`
- `OpenHydroQual/aquifolium/src/observation.cpp` (separate repo, uncommitted)
- `streaming_mcmc_algorithm.tex`, `proposal_adaptation_methods.tex`
- `deployments/Bioretention_{truth_drift,assimilation_MCMC_drift}/config.json`
