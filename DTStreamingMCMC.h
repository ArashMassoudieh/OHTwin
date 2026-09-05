/*
 * OpenHydroTwin
 * Copyright (C) 2026  Arash Massoudieh
 *
 * This file is part of OpenHydroTwin.
 *
 * OpenHydroTwin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenHydroTwin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

// ---------------------------------------------------------------------------
// DTStreamingMCMC
//
// Streaming Bayesian parameter assimilation for OpenHydroTwin.
//
// Subclasses the engine's batch sampler CMCMC<System> and converts it into
// a continuously operating, warm-started multi-chain Metropolis-Hastings
// ensemble that re-samples the kernel-weighted posterior at each
// calibration cycle. Reused from the base class unchanged:
//
//   - posterior(par, id)        : log-prior + kernel-weighted log-likelihood
//                                 (identical target to the deployed GA mode)
//   - pertcoeff / proposal init : per-parameter proposal scales
//   - SetParameters/SetProperty : configuration from the "MCMC" Settings
//                                 object in the model file
//
// Replaced by this class (the base versions are strided/batch/GUI-coupled
// and are never instantiated here):
//
//   - storage    : per-chain state (DTChainState) instead of a flat,
//                  pre-allocated Params array indexed by absolute sample no.
//   - driver     : wall-clock-bounded runCycle() instead of the fixed-length
//                  step(k, nsamps, filename, ProgressWindow*)
//   - burn-in    : per-chain, per-cycle plateau detection instead of the
//                  global burnout_samples discard
//
// Governing design principle (spec Section 1): selection among chains is
// free in the disposable pre-plateau phase and forbidden in the pool.
// All acceleration machinery (seeding, culling, dissolution) touches only
// pre-plateau chains; the pooled posterior is gated on convergence alone.
//
// Section references (Sec. x.y) throughout point to "Streaming Bayesian
// Parameter Assimilation for OpenHydroTwin: Methodology and Implementation
// Specification (v2)".
//
// Lifetime: constructed by DTAssimilation once per calibration cycle
// around the freshly prepared System (same pattern as CGA<System>), but
// cross-cycle state (previous pool, carried chain states, proposal scales)
// is persisted to / restored from the posterior snapshot file, so the
// object itself can be cycle-local.
// ---------------------------------------------------------------------------

#include "MCMC.h"
#include "System.h"
#include "TimeSeries.h"

#include <QString>
#include <QDateTime>

#include <deque>
#include <random>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// Streaming-only settings. Knobs the base _MCMC_settings already carries
// (number_of_chains, numberOfThreads, acceptance_rate, purt_change_scale,
// purturbation_factor) are NOT duplicated here; they continue to be parsed
// from the model's "MCMC" Settings object via CMCMC::SetParameters().
// ---------------------------------------------------------------------------
struct DTStreamingSettings
{
    // --- plateau classifier (Sec. 3.2) ---
    int    plateauWindow          = 60;    // trace length (per-chain steps) over which trend is tested
    double plateauSlopeThreshold  = 1.0;   // DECLARE plateau: |slope * window| < this many trace-stddevs
    double plateauRevertFactor    = 3.0;   // REVERT to climbing only above threshold*factor (hysteresis:
    // prevents flapping when trend/scatter hovers near 1.0)
    int    minStepsBeforeClassify = 20;    // chains younger than this are always "climbing"
    double minAcceptedFraction    = 0.05;  // a window with fewer accepted moves than this fraction of
    // its length is STAGNATION, never a plateau (a flat trace of
    // rejections is a stuck chain, not a converged one)

    // --- quorum (Sec. 3.2 / 3.9): primary certification criterion ---
    // A cycle is certified FULL iff at least this fraction q of chains reached
    // stationarity within the cycle. Within-cycle test, so it is compatible
    // with genuine cross-cycle parameter drift. Set from config
    // (mcmc_quorum_fraction). Lower it to certify a larger proportion of
    // cycles (fewer plateaued chains required).
    double quorumFraction         = 0.5;   // fraction q of chains that must be plateaued

    // --- inter-cycle stability certification (weaker fallback) ---
    // A second path to FULL for the streaming regime where a within-cycle
    // quorum cannot form but the point estimate is stationary across cycles:
    // the last stabilityWindow point estimates all moved < stabilityTol
    // (relative to their recent mean) AND >= stabilityMinPlateaued of chains
    // plateaued. This path PRESUMES a stationary target, so it is at odds with
    // parameter-drift detection; set stabilityEnabled=false to turn it off
    // entirely, or widen stabilityTol / shrink stabilityWindow to relax it.
    // All four are config-driven (mcmc_stability_*).
    bool   stabilityEnabled       = true;  // false => quorum is the sole criterion
    int    stabilityWindow        = 5;     // consecutive cycles compared
    double stabilityTol           = 0.02;  // max relative point-estimate drift
    double stabilityMinPlateaued  = 0.1;   // floor on plateaued fraction to trust the partial pool

    // --- accelerated burn-in cull (Sec. 3.5) ---
    bool   cullEnabled            = true;
    double cullMarginLogp         = 20.0;  // log-posterior units below plateaued ensemble median

    // --- dissolution of mechanically stalled chains (Sec. 3.7) ---
    int    stuckThreshold         = 200;   // consecutive rejections
    double minAcceptanceRate      = 1e-3;  // over the plateau window

    // --- cold start (Sec. 3.3) ---
    int    coldStartMultiplier    = 20;    // K = multiplier * Nc prior candidates
    double explorerFraction       = 0.03;  // chains seeded raw from the prior

    // --- proposal preconditioning (alternative to the global scale) ---
    // When true, propose X = x + kappa * L z with L = chol(Sigma), where
    // Sigma is a mean-centered posterior covariance ACCUMULATED across all
    // cycles (the correlation structure is model-determined and stationary
    // even as parameter values drift), built in the per-coordinate proposal
    // space (log for log-normal priors), and kappa a single adapted scalar.
    // false => the legacy per-coordinate global-scale random walk.
    bool   adaptiveCovariance    = false;  // config: mcmc_proposal_mode == "covariance"

    // --- anti-collapse safeguards ---
    // The warm-start pool seeds the next cycle, so any under-dispersion
    // compounds: measured pool spread fell 377x over 244 cycles while the
    // accumulated Sigma stayed ~250-1000x wider, and 95% CI coverage of the
    // known truth fell to 3-10%. Doubling the per-cycle sweep budget made it
    // strictly worse, which locates the contraction in the seeding step, not
    // in insufficient sampling.
    //
    // seedInflation: multiplicative ensemble inflation applied about the seed
    // mean in proposal space (EnKF-style). 1.0 = off (previous behaviour).
    double seedInflation          = 1.0;   // config: mcmc_seed_inflation

    // kappaMin: hard floor on the global proposal scalar. In the accel-100 run
    // kappa saturated at exactly this floor, i.e. the compensator gave up.
    double kappaMin               = 1e-4;  // config: mcmc_kappa_min

    // minStepFraction: floor the effective step kappa*sd_i at this fraction of
    // each parameter's prior width (median over parameters), so the proposal
    // cannot shrink arbitrarily far below the prior scale. 0 = off.
    double minStepFraction        = 0.0;   // config: mcmc_min_step_fraction

    // driftSeedInflation: ensemble inflation used by RatioReseed (the seeding
    // regime selected once drift is detected). After a drift the previous
    // pool is centred on a target that has moved, so the seeds need more
    // spread than the stationary seedInflation provides -- otherwise the
    // ensemble has to traverse to the relocated mode using a proposal whose
    // scale was tuned for the old one. Must be >= 1; values <= 1 fall back to
    // seedInflation.
    double driftSeedInflation     = 1.5;   // config: mcmc_drift_seed_inflation

    // --- drift detection ---
    // Two complementary tests on the per-cycle pooled means, both operating in
    // proposal space (log for log-normal priors) so a "20% change" means the
    // same thing for a conductivity as for a coefficient:
    //
    //  * CUSUM  -- online trigger. Accumulates standardized deviations with a
    //    slack k, alarms at h. Effect-size aware (noise below k never
    //    accumulates), so it fires fast without false alarms. NOT a hypothesis
    //    test: it yields a detection time, not a p-value.
    //  * Hotelling T2 -- reportable statistic. Two-sample test of the recent
    //    window against the reference window over the FULL parameter vector,
    //    giving a single p-value for "has anything drifted".
    //
    // Both treat the CYCLE as the replicate unit, never the individual draw:
    // with a 100-day window advancing 3 days, consecutive cycles share ~97% of
    // their data (measured lag-1 autocorrelation of cycle means 0.47,
    // integrated autocorrelation time ~8 cycles). Using raw draw counts gives
    // p-values wrong by tens of orders of magnitude.
    bool   driftDetectionEnabled  = false; // config: mcmc_drift_detection
    double cusumK                 = 1.0;   // slack, in reference SDs (= half the shift to detect)
    double cusumH                 = 5.0;   // alarm threshold
    int    driftReferenceCycles   = 40;    // cycles used to fix the in-control reference
    int    t2WindowCycles         = 33;    // cycles per T2 window (>= one calibration window)
    int    driftHistoryCap        = 200;   // cycle-mean ring retained for T2

    // --- cycle driver (Sec. 3.8) ---
    int    maxSweeps              = 0;     // hard cap on sweeps per cycle (0 = deadline only)
    int    stepsPerClockCheck     = 1;     // chain-sweeps between deadline checks
    int    adaptationBlock        = 20;    // sweeps between proposal-scale adaptations
    // (short: streaming cycles are ~100 sweeps and the target
    // sharpens every cycle as the record grows, so adaptation
    // must get several corrections per cycle to track it)

    // --- posterior predictive realizations (Sec. GUI ProduceRealizations) ---
    int    realizationCount       = 100;   // reservoir size: modeled outputs retained per cycle for
    // the predictive band (uniform random over pooled samples,
    // captured during sampling -- NOT re-solved afterward)
    int    realizationInterval    = 10;    // publish realization band + posterior distribution/CI
    // every Nth FULL cycle (0 disables)

    // --- diagnostics / IO ---
    bool   detailLogging          = false; // per-evaluation detail file writes (base-class behavior)
};

// ---------------------------------------------------------------------------
// Per-chain state (Sec. 3.2). Replaces the base class's strided flat arrays.
// ---------------------------------------------------------------------------
enum class ChainPhase { Climbing, Plateaued };

// ---------------------------------------------------------------------------
// Fixed-size reservoir of modeled outputs for the predictive band. Holds at
// most `capacity` per-observation modeled series, drawn uniformly at random
// (reservoir sampling) from the pooled post-plateau samples as they are
// retained during the sweep -- so the band costs no extra solves, and memory
// is O(capacity * observations * record), never O(samples). Shared across
// chains; a mutex guards offer() since retention happens in the parallel
// sweep. Each slot stores one realization's full set of observation series.
// ---------------------------------------------------------------------------
struct DTOutputReservoir
{
    // slot[r] = modeled series for observation o at outputs[r][o]
    std::vector<std::vector<TimeSeries<double>>> outputs;
    long long seen = 0;          // total retained samples offered (reservoir N)
    int  capacity  = 100;

    void reset(int cap)
    {
        capacity = std::max(1, cap);
        outputs.clear();
        seen = 0;
    }
};

struct DTChainState
{
    std::vector<double> params;          // current position psi = (theta, sigma)
    double              logp = -1e300;   // current log-posterior

    // classifier inputs
    std::deque<double>  trace;           // recent logp history (<= plateauWindow)
    std::deque<char>    acceptTrace;     // 1/0 accept flags, parallel to trace
    // (classifier stagnation guard; independent
    // of the per-block adaptation counters)
    ChainPhase          phase = ChainPhase::Climbing;
    int                 plateauOnsetStep = -1;  // sweep index at which plateau was declared

    // health bookkeeping (Sec. 3.7)
    int                 stuckCounter = 0;       // consecutive rejections
    int                 acceptedInWindow = 0;
    int                 proposedInWindow = 0;

    // retained (post-plateau) samples — the only samples that ever pool
    std::vector<std::vector<double>> samples;
    std::vector<double>              sampleLogp;

    bool explorer = false;               // seeded raw from the prior (Sec. 3.3)
    int  reseedCount = 0;                // times culled/dissolved this cycle

    // Full within-cycle history, for post-hoc analysis only -- never read by
    // the sampler. `trace` above is a rolling deque capped at plateauWindow,
    // so the climb->plateau trajectory is otherwise unrecoverable after the
    // cycle ends. Bounded by maxSweeps (~300), so ~2.4 kB per chain.
    std::vector<double> fullTraceLogp;
    std::vector<char>   fullTraceAccepted;   // 1 = this step was accepted
    std::vector<char>   fullTracePhase;      // 'C' climbing, 'P' plateaued
};

// ---------------------------------------------------------------------------
// Posterior summaries computed from a full (converged) pool (Sec. 3.9).
// ---------------------------------------------------------------------------
struct DTPosteriorSummary
{
    std::vector<double>              mean;
    std::vector<double>              stdev;
    std::vector<std::vector<double>> covariance;    // joint, np x np
    std::vector<double>              p025, p50, p975; // credible interval bounds
    std::vector<double>              p10, p90;         // GA-comparable band (viewer)
};

// ---------------------------------------------------------------------------
// Product of one calibration cycle (Sec. 3.9). Serialized by
// writePosteriorSnapshot(); consumed by DTAssimilation (point estimate ->
// calibrated System snapshot) and by the next cycle's seeding.
// ---------------------------------------------------------------------------
struct DTCycleResult
{
    bool converged = false;              // quorum reached => full snapshot

    // full payload (valid iff converged)
    std::vector<std::vector<double>> pooledSamples;
    std::vector<double>              pooledLogp;   // needed for ratio reseed (Sec. 3.3)
    DTPosteriorSummary               summary;

    // provisional payload (always valid; == chain states when !converged)
    std::vector<std::vector<double>> chainParams;
    std::vector<double>              chainLogp;

    // point estimate for the forward loop (both cases)
    std::vector<double>              pointEstimate;

    // diagnostics (Sec. 3.11)
    double plateauedFraction   = 0.0;
    double effectiveSampleSize = 0.0;

    // How a FULL publication was certified: "quorum", "stability", or
    // "" (provisional). Recorded in the snapshot/history for auditing.
    std::string convergenceSource;
    double acceptanceRate      = 0.0;
    int    cullCount           = 0;
    int    dissolutionCount    = 0;
    qint64 totalSweeps         = 0;
    qint64 totalEvaluations    = 0;
};

// Seeding regimes (Sec. 3.3, 3.9, Alg. 1 lines 3-11).
enum class SeedMode
{
    ColdStart,          // first cycle: importance-resampled from the prior
    WarmStart,          // stationary: uniform draw from previous pooled posterior
    RatioReseed,        // post-drift: resample previous pool prop. to pi_t/pi_{t-1}
    ProvisionalResume   // previous cycle was provisional: resume carried chain states
};

class DTStreamingMCMC : public CMCMC<System>
{
public:
    explicit DTStreamingMCMC(System *system);

    DTStreamingSettings streamSettings;

    // -----------------------------------------------------------------------
    // Cross-cycle state exchange (Sec. 3.9). Call loadPosteriorSnapshot()
    // before initializeCycle(); it populates the previous pool / carried
    // states / proposal scales and reports which SeedMode is appropriate.
    // -----------------------------------------------------------------------
    bool loadPosteriorSnapshot(const QString &path, QString &errorMessage);
    // Non-const: parameter(int) accessor (parameter names in the schema).
    bool writePosteriorSnapshot(const QString &path,
                                const DTCycleResult &result,
                                QString &errorMessage);

    // Posterior predictive realizations (mirrors the GUI's
    // CMCMC::ProduceRealizations): draw realizationCount parameter
    // vectors, solve a fresh model per draw (parallel, on the chain-
    // private pristine copies), and write the per-observation 2.5/50/
    // 97.5 percentile bracket to outputPath. Draws come from the pooled
    // posterior when converged, else from the carried chain states
    // (band is then chain-state dispersion, tagged via `converged`, not
    // posterior width). Raw realizations written only when writeRaw.
    // Returns false (with errorMessage) on hard failure; the caller
    // treats predictive output as best-effort, never a cycle failure.
    // Non-const: parameter(int) accessor + model solves.
    // Publish the predictive output band from the reservoir (no re-solves)
    // plus the parameter posterior distribution and CI, tagged by tNow.
    // Written only by the caller when the cycle is FULL and on the
    // realization interval. Best-effort: returns false on hard failure.
    bool produceRealizationCI(const DTCycleResult &result,
                              const QString &outputPath,
                              double tNow,
                              QString &errorMessage);

    // Per-parameter histogram (TimeSeries::distribution over pooled samples,
    // bins = clamp(round(sqrt(N)),10,60)) + a mean/stdev/p2.5/p50/p97.5 CI
    // table. Uses the full pooled samples, not the reservoir.
    bool writePosteriorDistribution(const DTCycleResult &result,
                                    const QString &outputPath,
                                    double tNow,
                                    QString &errorMessage);

    // Post-hoc analysis dumps, written by the caller on the same realization
    // interval as the two above. Both record state that is otherwise
    // destroyed at cycle end -- the accumulated proposal covariance (which
    // the snapshot overwrites) and the per-chain log-posterior trajectories
    // (DTChainState::trace is a rolling deque capped at plateauWindow).
    // Neither is ever read back by the sampler.
    bool writeProposalCovariance(const QString &outputPath, double tNow,
                                 QString &errorMessage);
    bool writeChainTraces(const QString &outputPath, double tNow,
                          QString &errorMessage);

    // Append one wide row per cycle to a running parameter-CI CSV
    // (mean/stdev/p2.5/p50/p97.5 per parameter). Called EVERY cycle, not
    // gated by the realization interval. Provisional cycles write the
    // point estimate into the mean/p50 columns and leave stdev/p025/p975
    // blank (no CI exists; transit dispersion is never emitted). Truncates
    // + writes the header when m_cycleIndex <= 1 (fresh deployment).
    bool appendParameterCIRow(const DTCycleResult &result,
                              const QString &csvPath,
                              double tNow,
                              QString &errorMessage);

    // Append this cycle's full pooled posterior samples to a cumulative CSV,
    // ONE ROW PER SAMPLE: cycle, t_now, converged, <param columns...>. Called
    // every cycle (provisional included) so the raw draws accumulate across
    // cycles for building smooth publication distributions -- the per-cycle
    // pool is otherwise discarded after the summary/histogram are computed.
    // The `cycle` column tags each sample with its origin so consumers can
    // pool over a chosen (e.g. stationary) window; naively pooling ALL cycles
    // mixes the sliding-window targets. Header written on cycle 1 or when the
    // file does not yet exist (feature enabled mid-run); appends otherwise, so
    // the record survives across cycles and restarts.
    bool appendPooledSamples(const DTCycleResult &result,
                             const QString &csvPath,
                             double tNow,
                             QString &errorMessage);

    // Append one compact JSON line for this cycle to the cumulative
    // history file consumed by the viewer (point estimate, posterior
    // percentiles, diagnostics -- never samples, so the file stays ~1 KB
    // per cycle). tNow is the calibration window end (OHQ day-serial).
    // The file is truncated when this is the deployment's first cycle
    // (m_cycleIndex == 1), mirroring archiveGAOutput. Provisional cycles
    // are recorded with converged=false and NO percentile fields, so
    // transit dispersion is structurally absent from the plot data.
    // Non-const: parameter(int) accessor.
    bool appendHistoryRecord(const QString &path,
                             const DTCycleResult &result,
                             double tNow,
                             QString &errorMessage);

    // Chooses among ColdStart / ProvisionalResume / RatioReseed / WarmStart
    // from what loadPosteriorSnapshot() found plus the drift flag
    // (Alg. 1 lines 3-11). Drift detection itself is Phase 3; until then
    // the caller passes driftDetected = false.
    SeedMode chooseSeedMode(bool driftDetected) const;

    // -----------------------------------------------------------------------
    // Cycle lifecycle (Alg. 1)
    // -----------------------------------------------------------------------

    // Seeds all chains per the given mode (Sec. 3.3 / 3.9) and resets
    // per-cycle bookkeeping. Evaluates the posterior at each seed.
    bool initializeCycle(SeedMode mode, QString &errorMessage);

    // The wall-clock-bounded sampling loop (Sec. 3.8, Alg. 1 lines 12-29):
    // sweeps all chains in parallel until the deadline, running the
    // classifier, cull, dissolution, and proposal adaptation as it goes,
    // then assembles either a full or a provisional result (Sec. 3.9).
    DTCycleResult runCycle(const QDateTime &deadline);

    // -----------------------------------------------------------------------
    // Point estimates for the forward loop (Sec. 3.9 / Alg. 1)
    // -----------------------------------------------------------------------
    std::vector<double> posteriorMean(const DTCycleResult &result) const;
    std::vector<double> posteriorMAP (const DTCycleResult &result) const;

    // Convenience: read-only view of the ensemble (diagnostics, tests).
    const std::vector<DTChainState> &chains() const { return m_chains; }

private:
    // -----------------------------------------------------------------------
    // Seeding internals (Sec. 3.3, 3.9) — one per SeedMode
    // -----------------------------------------------------------------------
    bool seedColdStart(QString &errorMessage);          // importance resampling from prior
    bool seedWarmStart(QString &errorMessage);          // uniform draw from previous pool
    bool seedRatioWeighted(QString &errorMessage);      // post-drift reseed
    bool seedFromCarriedStates(QString &errorMessage);  // provisional resume

    // Shared body of seedWarmStart/seedRatioWeighted: validate the previous
    // pool, draw one seed per chain uniformly with replacement, then inflate
    // about the seed mean at the given rate.
    bool seedFromPreviousPool(double inflationRate, QString &errorMessage);

    // Draw one overdispersed parameter vector: uniform over the range
    // (uniform in log space for log-normal priors) -- NOT the prior
    // density. Mirrors CMCMC::initialize(random=true). Used by cold start
    // and explorer chains; importance weights must therefore be the full
    // posterior value (log-prior + J), not the likelihood alone.
    // Non-const: parameter(int) accessor.
    std::vector<double> drawFromRange(std::mt19937_64 &rng);

    // -----------------------------------------------------------------------
    // Per-chain mechanics
    // -----------------------------------------------------------------------

    // Posterior evaluation against chain c's pristine model
    // (m_chainModels[c]): log-prior + kernel-weighted log-likelihood,
    // identical value to CMCMC::posterior but with a chain-private copy
    // source (race-free under the parallel sweep) and without the
    // per-evaluation detail-file critical section.
    // When captureOut != nullptr, the modeled observation series are copied
    // out of the transient work model before it is destroyed (used only for
    // retained plateaued samples that will be offered to the reservoir).
    double posteriorLocal(int c, const std::vector<double> &par,
                          std::vector<TimeSeries<double>> *captureOut = nullptr);

    // One MH step of chain c: propose (additive normal / multiplicative
    // log-normal per prior type, with the log-normal Jacobian in the
    // acceptance ratio, mirroring CMCMC::purturb/step), accept or reject
    // against the chain's own previous logp, update trace and health
    // counters, and append to the retained sample store iff plateaued.
    // Returns true if the proposal was accepted.
    bool stepChain(int c, std::mt19937_64 &rng);

    // Proposal draw around x. logJacobian receives the full Hastings
    // log-correction for multiplicative log-normal proposals
    // (sum of log X'_i - log x_i); 0 for all-normal parameter sets.
    // Non-const: the base accessor parameter(int) is non-const.
    std::vector<double> proposeFrom(const std::vector<double> &x,
                                    std::mt19937_64 &rng,
                                    double &logJacobian);

    // -----------------------------------------------------------------------
    // Plateau classifier and quorum (Sec. 3.2)
    // -----------------------------------------------------------------------

    // Trajectory-based (never level-based) classification of chain c from
    // its own logp trace: trend => Climbing, no trend => Plateaued.
    void classifyChain(int c, qint64 sweepIndex);

    double plateauedFraction() const;
    bool   quorumHolds() const;

    // Adaptive-covariance proposal (streamSettings.adaptiveCovariance).
    // accumulateProposalCovariance folds this cycle's pooled, mean-centered
    // covariance (in proposal space) into a running all-cycles estimate;
    // refactorProposalCholesky recomputes the ridged Cholesky factor used by
    // proposeFrom. The accumulator persists across cycles (never reset).
    void   accumulateProposalCovariance(const DTCycleResult &result);
    void   refactorProposalCholesky();

    // Inter-cycle stability test: true iff the ring is full and every
    // parameter's spread across the ring is below stabilityTol relative to
    // its recent mean. Called after this cycle's point estimate is pushed.
    bool   streamStable() const;

    // Median current logp over plateaued chains — the ensemble level the
    // cull margin is measured against (Sec. 3.5).
    double plateauedEnsembleLevel() const;

    // -----------------------------------------------------------------------
    // Interventions — pre-plateau phase only (Sec. 3.5, 3.7)
    // -----------------------------------------------------------------------

    // Cull: quorum holds AND chain climbing AND logp more than
    // cullMarginLogp below plateauedEnsembleLevel(). On trigger, clones a
    // uniformly random plateaued chain. Returns true if fired.
    bool cullCheck(int c, std::mt19937_64 &rng);

    // Dissolution: mechanical-health test (stuck counter / near-zero
    // acceptance / non-finite logp). Never keyed on posterior level.
    // On trigger, clones a uniformly random healthy chain. Returns true
    // if fired.
    bool dissolutionCheck(int c, std::mt19937_64 &rng);

    // Copy src's current position/logp into dst; reset dst's trace,
    // phase (=> Climbing), health counters, and plateau onset so it must
    // re-plateau before contributing samples (Sec. 3.5).
    void cloneChain(int src, int dst);

    int randomPlateauedChain(std::mt19937_64 &rng) const;  // -1 if none
    int randomHealthyChain  (std::mt19937_64 &rng) const;  // -1 if none

    // -----------------------------------------------------------------------
    // Adaptation, pooling, diagnostics
    // -----------------------------------------------------------------------

    // Global pertcoeff adaptation toward MCMC_Settings.acceptance_rate,
    // reusing the base class's purt_change_scale rule, driven from the
    // per-chain accept counters (Sec. 3.1).
    void adaptProposalScale();

    // Pool post-plateau samples across chains, equally weighted
    // (Alg. 1 line 25), and fill the full-payload fields of result.
    void assemblePool(DTCycleResult &result) const;

    DTPosteriorSummary computeSummary(
        const std::vector<std::vector<double>> &samples) const;

    // Autocorrelation-based ESS, computed PER CHAIN (the pool is a
    // concatenation of chains, so autocorrelation on the flat stream
    // would see false correlation at chain seams). Geyer initial-
    // monotone-sequence truncation; ESS summed across chains per
    // parameter, reported as the minimum across parameters -- the
    // worst-mixing direction bounds the pool's reliability (Sec. 3.11).
    double effectiveSampleSize() const;

    // One structured log line per cycle (Sec. 3.11).
    void logCycleDiagnostics(const DTCycleResult &result) const;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    std::vector<DTChainState> m_chains;

    // One pristine System per chain, copied SERIALLY from the shared
    // model at cycle initialization (initializeCycle). Each evaluation
    // copies from its own chain's pristine object (copy-solve-discard,
    // the same semantics as CMCMC::posterior) -- so no two threads ever
    // touch the same System concurrently, not even as a copy source.
    // Memory: nc pristine copies + up to numberOfThreads transient
    // working copies resident during a sweep.
    std::vector<System> m_chainModels;

    // Predictive-band output reservoir (Sec: realizations). Reset each cycle.
    DTOutputReservoir m_outputReservoir;
    std::mutex        m_reservoirMutex;

    // Offer one retained sample's modeled outputs to the reservoir. Called
    // from stepChain under the parallel sweep for plateaued samples only.
    // Takes ownership by move. Reservoir-sampling: fills to capacity, then
    // replaces a random slot with probability capacity/seen.
    void offerToReservoir(std::vector<TimeSeries<double>> &&modeled);

    // Previous cycle's product, restored by loadPosteriorSnapshot():
    bool m_havePrevSnapshot   = false;
    bool m_prevWasProvisional = false;
    std::vector<std::vector<double>> m_prevSamples;  // pooled samples (full) or chain states (provisional)
    std::vector<double>              m_prevLogp;     // their posterior values (for ratio reseed)
    std::vector<double>              m_prevPertcoeff; // proposal scales carried across cycles

    // Adaptive-covariance state carried across cycles. DTStreamingMCMC is
    // constructed fresh every calibration cycle, so the running covariance
    // only accumulates if it round-trips through the posterior snapshot;
    // without this it was rebuilt from one cycle and discarded, and
    // m_propReady was never true for a single proposal.
    std::vector<std::vector<double>> m_prevPropCov;
    double m_prevPropCovWeight = 0.0;
    double m_prevKappa         = 0.0;

    // Seeding regime actually used this cycle, recorded for the history file.
    SeedMode m_lastSeedMode = SeedMode::ColdStart;
    static const char *seedModeName(SeedMode m);

    // Multiplicative ensemble inflation about the seed mean, in proposal
    // space. No-op when the rate is <= 1.
    void inflateSeedEnsemble(double rate);

    // --- drift detection (persisted across cycles via the snapshot) ---
    std::vector<double> m_cusumSp, m_cusumSm;       // per-parameter CUSUM arms
    std::vector<double> m_driftRefMean, m_driftRefSd;
    int    m_driftRefCount   = 0;                  // cycles folded into the reference
    std::vector<std::vector<double>> m_cycleMeans; // ring: [t_now, mean_0..mean_{d-1}]
    bool   m_driftDetected   = false;
    double m_cusumMax        = 0.0;
    double m_t2PValue        = -1.0;               // -1 => not computable yet
    std::vector<int> m_driftIdx;                   // parameters whose CUSUM alarmed

    // Fold this cycle's pooled mean into the reference / CUSUM / T2 machinery.
    void updateDriftDetection(const DTCycleResult &result, double tNow);
    // Pooled mean of this cycle in proposal space; false if no usable pool.
    bool cycleMeanInProposalSpace(const DTCycleResult &result,
                                  std::vector<double> &out);
    // Integrated autocorrelation time (in cycles) of the stored cycle means.
    double cycleMeanAutocorrTime() const;
    // Two-window Hotelling T2 p-value; -1 when there is insufficient history.
    double hotellingT2PValue();

    // Scoring-window end for the current cycle (OHQ day serial). Set by the
    // caller before runCycle; used to timestamp the drift-detector history.
    double m_lastTNow = 0.0;

public:
    void setCurrentTime(double tNow) { m_lastTNow = tNow; }

    // True when the previous cycle's detector alarmed. Gates RatioReseed and
    // any drift-triggered response (window shortening, inflation boost).
    bool driftDetected() const { return m_driftDetected; }
    double cusumStatistic() const { return m_cusumMax; }
    double driftPValue() const { return m_t2PValue; }

private:

    // Lower bound on kappa: max(kappaMin, the kappa at which the median of
    // kappa*sd_i / priorWidth_i reaches minStepFraction). Returns kappaMin
    // when the step floor is disabled or Sigma is not yet available.
    double proposalScaleFloor();

    // Prior width of parameter i in proposal space (log range for log-normal).
    double priorWidthInProposalSpace(unsigned int i);

    // Inter-cycle stability ring: the last stabilityWindow point estimates,
    // oldest-first. Feeds streamStable().
    std::deque<std::vector<double>> m_pointEstimateHistory;

    // Adaptive-covariance proposal state (persists across cycles; not reset in
    // initializeCycle). m_propCov is the running mean-centered posterior
    // covariance in proposal space; m_propCovWeight the accumulated sample
    // weight; m_propChol its ridged Cholesky factor; m_kappa the global scale.
    std::vector<std::vector<double>> m_propCov;
    std::vector<std::vector<double>> m_propChol;
    double m_propCovWeight = 0.0;
    bool   m_propReady     = false;
    double m_kappa         = 0.0;   // lazily set to 2.38/sqrt(d)

    // Per-cycle bookkeeping
    qint64 m_sweepIndex        = 0;
    qint64 m_acceptedTotal     = 0;   // cycle totals, accumulated by
    qint64 m_proposedTotal     = 0;   // adaptProposalScale() at each block
    int    m_cullCount         = 0;
    int    m_dissolutionCount  = 0;
    qint64 m_evaluationCount   = 0;
    int    m_cycleIndex        = 0;

    std::mt19937_64 m_seedRng;  // seeded in the constructor; used for seeding
    // and clone-target draws (per-thread RNGs for
    // the parallel sweep live inside runCycle)
};
