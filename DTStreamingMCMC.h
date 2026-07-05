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

#include <QString>
#include <QDateTime>

#include <deque>
#include <random>
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
    int    plateauWindow          = 40;    // trace length (per-chain steps) over which trend is tested
    double plateauSlopeThreshold  = 1.0;   // |slope * window| below this many trace-stddevs => plateaued
    int    minStepsBeforeClassify = 10;    // chains younger than this are always "climbing"

    // --- quorum (Sec. 3.2 / 3.9) ---
    double quorumFraction         = 0.5;   // fraction q of chains that must be plateaued

    // --- accelerated burn-in cull (Sec. 3.5) ---
    bool   cullEnabled            = true;
    double cullMarginLogp         = 20.0;  // log-posterior units below plateaued ensemble median

    // --- dissolution of mechanically stalled chains (Sec. 3.7) ---
    int    stuckThreshold         = 200;   // consecutive rejections
    double minAcceptanceRate      = 1e-3;  // over the plateau window

    // --- cold start (Sec. 3.3) ---
    int    coldStartMultiplier    = 20;    // K = multiplier * Nc prior candidates
    double explorerFraction       = 0.03;  // chains seeded raw from the prior

    // --- cycle driver (Sec. 3.8) ---
    int    stepsPerClockCheck     = 1;     // chain-sweeps between deadline checks
    int    adaptationBlock        = 50;    // sweeps between proposal-scale adaptations

    // --- diagnostics / IO ---
    bool   detailLogging          = false; // per-evaluation detail file writes (base-class behavior)
};

// ---------------------------------------------------------------------------
// Per-chain state (Sec. 3.2). Replaces the base class's strided flat arrays.
// ---------------------------------------------------------------------------
enum class ChainPhase { Climbing, Plateaued };

struct DTChainState
{
    std::vector<double> params;          // current position psi = (theta, sigma)
    double              logp = -1e300;   // current log-posterior

    // classifier inputs
    std::deque<double>  trace;           // recent logp history (<= plateauWindow)
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
    bool seedRatioWeighted(QString &errorMessage);      // prop. to pi_t/pi_{t-1}
    bool seedFromCarriedStates(QString &errorMessage);  // provisional resume

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

    // Autocorrelation-based ESS of the pooled draws; reported as the
    // minimum across parameters (Sec. 3.11).
    double effectiveSampleSize(
        const std::vector<std::vector<double>> &samples) const;

    // One structured log line per cycle (Sec. 3.11).
    void logCycleDiagnostics(const DTCycleResult &result) const;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    std::vector<DTChainState> m_chains;

    // Previous cycle's product, restored by loadPosteriorSnapshot():
    bool m_havePrevSnapshot   = false;
    bool m_prevWasProvisional = false;
    std::vector<std::vector<double>> m_prevSamples;  // pooled samples (full) or chain states (provisional)
    std::vector<double>              m_prevLogp;     // their posterior values (for ratio reseed)
    std::vector<double>              m_prevPertcoeff; // proposal scales carried across cycles

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
