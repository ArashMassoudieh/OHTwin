# Session handoff — streaming MCMC assimilation + viewer (2026-07-06 → 07)

Work on the `DrywellDT` streaming-MCMC assimilation runner and the WASM viewer.

## Done and built
Runner binary: `build-qmake/bin/OHTwin`; viewer: native (`Desktop_Qt_6_8_2-Debug`) and WASM (`WebAssembly_Qt_6_8_2_multi_threaded-Release`). All items below are compiled in.

### Day 1 (viewer + provisional outputs)
- **Realization CI / posterior dist were never produced.** Relaxed the convergence gates in `DTStreamingMCMC.cpp` (`produceRealizationCI`, `writePosteriorDistribution`) and the schedule gate in `DTAssimilation.cpp` (`runCalibrationMCMC`) so they publish provisionally, not only when `result.converged`.
- **Realization CI is now ONE combined file** `realization_ci_latest.txt` (+ cycle-tagged), columns `<obs> | <pct>`, filtered to calibration observations. Fixed a silent bug: observation names containing `/` (e.g. `Underdrain flow (m3/day)`) broke the old per-observation filenames.
- **Provisional 2.5–97.5% parameter band.** `appendHistoryRecord` writes `mean/p10/p90/p025/p975` for every pooled cycle; viewer `PosteriorHistoryLoader` maps them regardless of `converged`; `AssimViewer` band uses `p025/p975`; caption says 2.5–97.5%.
- **New viewer Posterior tab.** `PosteriorDistLoader.{h,cpp}` + `DistPanel` histograms from `posterior_dist_latest.txt`; URL derived from `modeled_csv_url`. Also fixed: `m_realizationLoader.fetch()` was never called — now triggered in `onRefreshClicked`.
- **Doc:** `streaming_mcmc_algorithm.tex` (repo root) — algorithm reference (plateau/quorum/stability criteria, kernel likelihood, ESS, reservoir; notation table; flags stubbed features: cull/dissolution, ColdStart importance-resampling, RatioReseed).

### Day 2 (performance + robustness) — the important structural work
- **Root cause of the sweep collapse: unbounded calibration window.** `tStart = m_buffer.tMin()` never advanced, so each cycle re-solved `[t0, tEnd]` (grew 4→675 days); each MCMC step is one solve, so sweeps collapsed 433→3 and pools → 0 by ~cycle 73. The forgetting kernel down-weights old data statistically but the solver still integrated the whole window.
- **Rolling window + MAP spin-up (Path A).** `DTAssimilation`: scoring window is now `[tEnd - calibration_window_days, tEnd]`. When that start > overall start, `buildSpinupSnapshot` runs ONE forward solve `t0 → tStart`, cold-started from the `.ohq` script with the **latest MAP params**, captures end state (`SavetoJson calculatevalue=true`) to `_spinup_ic.json`; the calibration loads THAT as its base so all chains share a fixed MAP-consistent IC. Observed data clamped to `[tStart, tEnd]`. Weather factored into `injectCalibrationWeather` (reused by spin-up). **Safe degrade:** any spin-up failure falls back to full window.
- **Config knobs (`DTConfig`, in `config.json` `assimilation`):** `calibration_window_days` (default 365), `mcmc_max_sweeps` (default 300). Both optional; fall back to defaults if absent.
- **Max-sweeps cap** (`DTStreamingMCMC::runCycle`): stops at deadline OR `maxSweeps`, whichever first.
- **Weather-fetch timeout (DONE).** `noaaweatherfetcher.cpp` all 5 blocking `QEventLoop::exec()` GET sites now `setTransferTimeout(30000)` — a hung OpenMeteo (HTTPS/443) fetch errors out instead of freezing the event loop. Fixes the truth-stall bug and protects the spin-up's `[t0,tStart]` fetch.
- **Seamless resume (`DTRunner.cpp`).** Startup order is now: (1) resume from newest `state/state_*.json`'s `_dt_next_start_utc`; (2) else cold-start at `start_datetime`; (3) else now. `--fresh` wipes `state/` first, so **without `--fresh` = resume, with `--fresh` = start over**. Applies to both truth and MCMC (same DTRunner). Calibration side was already resume-safe (posterior warm-start + restored `m_cycleIndex` → history appends not truncates).

## Current run status (as of 2026-07-07)
- Running at `time_acceleration: 200`, `calibration_window_days: 100`, `mcmc_max_sweeps: 300`. One truth + one MCMC instance.
- **Healthy:** ~7 cycles in, every cycle hits 300 sweeps, pools 245–1469 samples, ESS 37–121. Big improvement over the collapse.
- Rolling window/spin-up **not triggered yet** — activates when `t_now` passes day 100 (`43933`), ~cycle 33. `_spinup_ic.json` will appear then.
- Still `converged:false` (plateaued 0.25–0.38 < 0.5 quorum) — separate threshold question.

## Pending / verify next (rough priority)
1. **Validate the spin-up at ~cycle 33.** Confirm: `debug.log` shows `rolling window: spun up IC at t=…` (no fallback warning); `_spinup_ic.json` appears/refreshes; **pool_size holds** (doesn't collapse) as the window caps at 100 d; and the spin-up IC is continuous (soil moisture / pond depth at tStart not reset to cold defaults). Watch whether `sweeps` stays at 300 as the window grows toward 100 d — if it dips before cycle 33, the deadline bound (window got expensive), then rolling caps it.
2. **Convergence never certifies** (`converged=true`) — quorum (0.5) / stability thresholds vs 16 chains. Tuning question.
3. Optional robustness not yet done: atomic writes (temp+rename) for `posterior_latest.json`/state snapshots (kill mid-write); restore `m_cyclesCompleted` on restart (currently resets 0 — harmless to history, misnumbers realization gate/logs).

## Gotchas
- Run exactly **one** MCMC + **one** truth instance. Duplicate `--deployment ...MCMC` processes race/wipe `outputs/` and double-write history. To resume: relaunch **without** `--fresh`.
- `t_now` in calibration outputs = `m_buffer.tMax()` (`DTAssimilation.cpp`) = latest observation time from truth; advances only when truth publishes newer data.
- Config `config.json` now carries `calibration_window_days`/`mcmc_max_sweeps` explicitly (shows as modified in git alongside 8 source files).

## Key files / paths
- `DTStreamingMCMC.{h,cpp}`, `DTAssimilation.{cpp,h}`, `DTConfig.{cpp,h}`, `DTRunner.cpp`, `noaaweatherfetcher.cpp`
- `viewer/AssimViewer.{h,cpp}`, `viewer/PosteriorHistoryLoader.cpp`, `viewer/RealizationCILoader.{h,cpp}`, `viewer/PosteriorDistLoader.{h,cpp}`, `viewer/OHTwinViewer.pro`
- Deployments: `deployments/Bioretention_assimilation_MCMC` (MCMC, outputs on port 8182), `deployments/Bioretention_truth` (truth on 8084)
- Algorithm doc: `streaming_mcmc_algorithm.tex`
