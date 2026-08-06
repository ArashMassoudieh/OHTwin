# Inflation + drift-detection test run

## What this tests

The completed run (`Bioretention_assimilation_MCMC_drift`) fixed the sampler
mechanics — acceptance 0.0086 → 0.166, ESS 24 → 73, convergence 0/244 → 172/244 —
but the **inference got worse**: 95% CI coverage of the known truth fell to 10%,
CI widths to ~0.5%, and Ksat ended at 6.17 against a truth of 2.0, never
recovering. The cause is ensemble collapse: pool spread fell **377×** while the
accumulated Σ̂ stayed 250–1000× wider, so a single scalar κ was left to absorb a
mismatch it structurally cannot.

Decisive evidence that it is a *contraction*, not undersampling: the accel-100
run doubled the sweeps per cycle and made everything worse — CI width 6.6×
tighter, coverage 10% → 3%, final Ksat error +209% → +278%. More sampling drives
the collapse further. That locates it in the **warm-start seeding**, which draws
each cycle's chains from the previous cycle's (already narrow) pool.

**Intervention under test:** `mcmc_seed_inflation: 1.05` — multiplicative
ensemble inflation about the seed mean, in proposal space. Measured contraction
is ×0.976/cycle; r=1.05 gives a net ×1.025, the smallest value that reverses the
sign.

Drift detection (CUSUM + Hotelling T²) is enabled as **passive instrumentation** —
it records but does not act, so inflation remains the single intervention.

## Everything else is identical to the completed run

`time_acceleration: 200` (deliberately **not** 100 — more sweeps amplify the
collapse), window 100 d, `max_sweeps` 300, `mcmc_proposal_mode: covariance`,
same model, same truth. Only the name, port (8185) and the new knobs differ,
so `Bioretention_assimilation_MCMC_drift` is a clean control.

## Running it

The truth must run fresh, in lockstep. Its previous outputs are already backed
up at `deployments/Bioretention_truth_drift_RUN1_outputs/`.

Terminal 1 — truth:

```bash
cd /home/arash/Projects/DrywellDT && ./build-qmake/bin/OHTwin --deployment deployments/Bioretention_truth_drift --fresh --force
```

Wait ~30 s for `selected_output.csv` to appear and start growing, then

Terminal 2 — assimilator:

```bash
cd /home/arash/Projects/DrywellDT && ./build-qmake/bin/OHTwin --deployment deployments/Bioretention_assimilation_MCMC_drift_INFLATE --fresh --force
```

Both need `--fresh` or they quit immediately (resume anchors sit past
`stop_datetime`). Full run ≈ 244 cycles × 21.6 min ≈ **88 h**, but the primary
question is answerable much sooner — see below.

## Checkpoints

```bash
python3 deployments/Bioretention_assimilation_MCMC_drift_INFLATE/check.py
```

**~cycle 20 (7 h) — is inflation active?**
`debug.log` should show `seed inflation: r=1.05 applied to 16 chains` from cycle 2.

**~cycle 40–60 (14–22 h) — THE test.** Pool spread should *stabilise* rather than
fall. In the control it was already down ~100× by cycle 40 and reached ~0.0004.
If it is still falling geometrically here, r=1.05 is too weak — stop and retry at
1.10. If it holds near 0.05–0.15, the mechanism is fixed.

Also watch the Σ̂/pool ratio: in the control it ran 200–1000. Near 1–5 means the
preconditioner is tracking the posterior and κ should sit near κ₀ = 0.79 instead
of saturating at its 1e-4 floor.

**~cycle 66+ — drift detector arms.** The reference period (40 cycles) has closed
and T² has two full windows. `drift_t2_p` should be > 0.05 and `cusum_max` below
h=5 through the pre-drift stretch; any alarm before t=44166 is a false positive.

**~cycle 111–121 (t=44166–44197) — the drift.** CUSUM fired 19 days after onset
on the baseline; T² needs ~100 days. Compare against those.

**End — the real scorecard.** Coverage of truth by the 95% CI: control 10%,
nominal 95%. Anything above ~50% is a substantive win; still under ~20% means
inflation alone is insufficient and the next lever is `save_interval` 1hr → 6hr
(the likelihood treats 6-hour-correlated noise as independent at hourly
resolution, which is what makes the posterior spuriously sharp in the first
place).

## Caveats

- Not runtime-tested. Inflation first runs at cycle 2; the detector needs 40+
  cycles for its reference and 66+ for T². A short smoke test cannot exercise
  either — treat the first hours as the test.
- The truth's noise realization is seeded from `random_device`, so observations
  will differ from run 1. Parameter-level comparisons are unaffected; per-timestep
  observation comparisons are not directly comparable.
- Port 8185 is not nginx-served, so no viewer. The run does not need it.
