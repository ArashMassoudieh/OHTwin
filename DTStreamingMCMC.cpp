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

#include "DTStreamingMCMC.h"
#include "DTDebugLog.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>

#ifndef NO_OPENMP
#include <omp.h>
#endif

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
DTStreamingMCMC::DTStreamingMCMC(System *system)
    : CMCMC<System>(system)
{
    // Base ctor has already:
    //   - set Model = system
    //   - registered all Parameters (number_of_parameters, params[])
    //   - wired parameters/observations pointers
    //   - sized pertcoeff
    //
    // Deterministic-but-distinct seed per process start; per-thread RNGs
    // for the parallel sweep are derived from m_seedRng in runCycle().
    std::random_device rd;
    m_seedRng.seed((static_cast<std::uint64_t>(rd()) << 32) ^ rd());

    // Streaming mode never uses the base class's pre-allocated sample
    // storage; keep it empty. total_number_of_samples is retired (Sec. 3.8).
    MCMC_Settings.total_number_of_samples = 0;
}

// ---------------------------------------------------------------------------
// Cross-cycle state exchange (Sec. 3.9)
// ---------------------------------------------------------------------------
namespace
{
// JSON <-> vector/matrix helpers for the posterior snapshot schema.
QJsonArray toJsonArray(const std::vector<double> &v)
{
    QJsonArray a;
    for (double x : v) a.append(x);
    return a;
}

QJsonArray toJsonMatrix(const std::vector<std::vector<double>> &m)
{
    QJsonArray a;
    for (const std::vector<double> &row : m) a.append(toJsonArray(row));
    return a;
}

std::vector<double> fromJsonArray(const QJsonArray &a)
{
    std::vector<double> v;
    v.reserve(a.size());
    for (const QJsonValue &x : a) v.push_back(x.toDouble());
    return v;
}

std::vector<std::vector<double>> fromJsonMatrix(const QJsonArray &a)
{
    std::vector<std::vector<double>> m;
    m.reserve(a.size());
    for (const QJsonValue &row : a) m.push_back(fromJsonArray(row.toArray()));
    return m;
}
} // namespace

bool DTStreamingMCMC::loadPosteriorSnapshot(const QString &path,
                                            QString &errorMessage)
{
    m_havePrevSnapshot   = false;
    m_prevWasProvisional = false;
    m_prevSamples.clear();
    m_prevLogp.clear();
    m_prevPertcoeff.clear();

    // An absent snapshot is not an error -- it is the definition of a
    // cold start (first cycle of a deployment, or after a reset).
    if (path.isEmpty() || !QFile::exists(path))
        return true;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        errorMessage = "cannot open posterior snapshot: " + path;
        return false;
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    file.close();
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
    {
        errorMessage = "posterior snapshot is not valid JSON ("
                       + parseErr.errorString() + "): " + path;
        return false;
    }
    const QJsonObject root = doc.object();

    // Parameter-name audit. A mismatch means the model's estimated
    // parameter set changed between cycles (model file edited, parameter
    // added/removed/reordered). The previous pool is then meaningless for
    // seeding, but that is a reason to cold-start, not to fail the cycle.
    const unsigned int np = MCMC_Settings.number_of_parameters;
    const QJsonArray names = root.value("parameter_names").toArray();
    bool namesMatch = (static_cast<unsigned int>(names.size()) == np);
    if (namesMatch)
    {
        for (unsigned int i = 0; i < np; ++i)
        {
            if (names[static_cast<int>(i)].toString().toStdString()
                != parameter(i)->GetName())
            {
                namesMatch = false;
                break;
            }
        }
    }
    if (!namesMatch)
    {
        std::cerr << "[StreamMCMC] posterior snapshot parameter set does "
                     "not match the model -- ignoring snapshot, will "
                     "cold-start\n";
        return true;
    }

    m_prevWasProvisional = !root.value("converged").toBool(false);
    m_cycleIndex         = root.value("cycle").toInt(m_cycleIndex);

    // Proposal scales carry across cycles when dimensionally valid.
    std::vector<double> pc = fromJsonArray(root.value("pertcoeff").toArray());
    if (pc.size() == np)
        m_prevPertcoeff = std::move(pc);

    // Full snapshot -> pooled posterior samples; provisional -> carried
    // chain states. Both land in m_prevSamples/m_prevLogp; SeedMode
    // decides how they are used (Sec. 3.3 / 3.9).
    const char *samplesKey = m_prevWasProvisional ? "chain_params" : "samples";
    const char *logpKey    = m_prevWasProvisional ? "chain_logp"   : "sample_logp";
    m_prevSamples = fromJsonMatrix(root.value(samplesKey).toArray());
    m_prevLogp    = fromJsonArray(root.value(logpKey).toArray());

    // Structural validation; anything inconsistent degrades to cold start.
    bool valid = !m_prevSamples.empty()
                 && m_prevLogp.size() == m_prevSamples.size();
    if (valid)
    {
        for (const std::vector<double> &s : m_prevSamples)
        {
            if (s.size() != np) { valid = false; break; }
        }
    }
    if (!valid)
    {
        std::cerr << "[StreamMCMC] posterior snapshot payload is empty or "
                     "inconsistent -- ignoring, will cold-start\n";
        m_prevSamples.clear();
        m_prevLogp.clear();
        return true;
    }

    m_havePrevSnapshot = true;

    {
        DTDebugLog &dlog = DTDebugLog::instance();
        if (dlog.enabled(DTDebugLog::Category::Snapshot))
            dlog.log(DTDebugLog::Category::Snapshot,
                     QString("posterior loaded: %1 (%2, %3 rows, cycle=%4, "
                             "pertcoeff %5)")
                         .arg(path)
                         .arg(m_prevWasProvisional ? "provisional" : "full")
                         .arg(m_prevSamples.size())
                         .arg(m_cycleIndex)
                         .arg(m_prevPertcoeff.empty() ? "absent" : "carried"));
    }
    return true;
}

bool DTStreamingMCMC::writePosteriorSnapshot(const QString &path,
                                             const DTCycleResult &result,
                                             QString &errorMessage)
{
    QJsonObject root;
    root["converged"] = result.converged;
    root["cycle"]     = m_cycleIndex;

    const unsigned int np = MCMC_Settings.number_of_parameters;
    QJsonArray names;
    for (unsigned int i = 0; i < np; ++i)
        names.append(QString::fromStdString(parameter(i)->GetName()));
    root["parameter_names"] = names;

    root["pertcoeff"]      = toJsonArray(pertcoeff);
    root["point_estimate"] = toJsonArray(result.pointEstimate);

    if (result.converged)
    {
        // Full payload (Sec. 3.9): the pool, its posterior values (needed
        // by the ratio-weighted reseed), and the summaries.
        root["samples"]     = toJsonMatrix(result.pooledSamples);
        root["sample_logp"] = toJsonArray(result.pooledLogp);

        QJsonObject summary;
        summary["mean"]  = toJsonArray(result.summary.mean);
        summary["stdev"] = toJsonArray(result.summary.stdev);
        summary["cov"]   = toJsonMatrix(result.summary.covariance);
        summary["p025"]  = toJsonArray(result.summary.p025);
        summary["p50"]   = toJsonArray(result.summary.p50);
        summary["p975"]  = toJsonArray(result.summary.p975);
        summary["p10"]   = toJsonArray(result.summary.p10);
        summary["p90"]   = toJsonArray(result.summary.p90);
        root["summary"]  = summary;
    }
    else
    {
        // Provisional payload: carried chain states only. Deliberately NO
        // summary block -- mid-transit dispersion must never be readable
        // as posterior width by any downstream consumer (Sec. 3.9).
        root["chain_params"] = toJsonMatrix(result.chainParams);
        root["chain_logp"]   = toJsonArray(result.chainLogp);
    }

    QJsonObject diag;
    diag["plateaued_fraction"] = result.plateauedFraction;
    diag["ess"]                = result.effectiveSampleSize;
    diag["acceptance_rate"]    = result.acceptanceRate;
    diag["cull_count"]         = result.cullCount;
    diag["dissolution_count"]  = result.dissolutionCount;
    diag["sweeps"]             = static_cast<double>(result.totalSweeps);
    diag["evaluations"]        = static_cast<double>(result.totalEvaluations);
    root["diagnostics"] = diag;

    // Atomic publication: the file is read by the next cycle and may be
    // served to the viewer, so a half-written state must never be
    // observable. QSaveFile writes to a temporary and commits by rename.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
    {
        errorMessage = "cannot open posterior snapshot for writing: " + path;
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!out.commit())
    {
        errorMessage = "failed to commit posterior snapshot: " + path;
        return false;
    }

    {
        DTDebugLog &dlog = DTDebugLog::instance();
        if (dlog.enabled(DTDebugLog::Category::Snapshot))
            dlog.log(DTDebugLog::Category::Snapshot,
                     QString("posterior written: %1 (%2, samples=%3, "
                             "chains=%4)")
                         .arg(path)
                         .arg(result.converged ? "full" : "provisional")
                         .arg(result.pooledSamples.size())
                         .arg(result.chainParams.size()));
    }
    return true;
}

// ---------------------------------------------------------------------------
bool DTStreamingMCMC::appendHistoryRecord(const QString &path,
                                          const DTCycleResult &result,
                                          double tNow,
                                          QString &errorMessage)
{
    QJsonObject rec;
    rec["cycle"]     = m_cycleIndex;
    rec["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    rec["t_now"]     = tNow;
    rec["converged"] = result.converged;

    const unsigned int np = MCMC_Settings.number_of_parameters;
    QJsonArray names;
    for (unsigned int i = 0; i < np; ++i)
        names.append(QString::fromStdString(parameter(i)->GetName()));
    rec["parameter_names"] = names;

    rec["point_estimate"] = toJsonArray(result.pointEstimate);

    // Percentile fields exist ONLY for converged cycles: a provisional
    // record carries no dispersion at all, so no downstream plot can
    // render transit spread as a credible band (Sec. 3.9).
    if (result.converged)
    {
        rec["mean"] = toJsonArray(result.summary.mean);
        rec["p10"]  = toJsonArray(result.summary.p10);
        rec["p90"]  = toJsonArray(result.summary.p90);
        rec["p025"] = toJsonArray(result.summary.p025);
        rec["p975"] = toJsonArray(result.summary.p975);
    }

    rec["ess"]                = result.effectiveSampleSize;
    rec["plateaued_fraction"] = result.plateauedFraction;
    rec["acceptance_rate"]    = result.acceptanceRate;
    rec["pool_size"]          = static_cast<double>(result.pooledSamples.size());
    rec["sweeps"]             = static_cast<double>(result.totalSweeps);
    rec["evaluations"]        = static_cast<double>(result.totalEvaluations);

    // First cycle of a fresh deployment truncates (mirrors
    // archiveGAOutput); resumed deployments (cycle restored from the
    // posterior snapshot) append, preserving history across restarts.
    QFile file(path);
    const QIODevice::OpenMode mode =
        QIODevice::WriteOnly |
        (m_cycleIndex <= 1 ? QIODevice::Truncate : QIODevice::Append);
    if (!file.open(mode))
    {
        errorMessage = "cannot open posterior history for writing: " + path;
        return false;
    }
    file.write(QJsonDocument(rec).toJson(QJsonDocument::Compact));
    file.write("\n");
    file.close();

    {
        DTDebugLog &dlog = DTDebugLog::instance();
        if (dlog.enabled(DTDebugLog::Category::Snapshot))
            dlog.log(DTDebugLog::Category::Snapshot,
                     QString("history record appended: %1 (cycle=%2, %3)")
                         .arg(path).arg(m_cycleIndex)
                         .arg(result.converged ? "full" : "provisional"));
    }
    return true;
}

SeedMode DTStreamingMCMC::chooseSeedMode(bool driftDetected) const
{
    // Alg. 1 lines 3-11:
    //   no previous snapshot        -> ColdStart
    //   previous was provisional    -> ProvisionalResume
    //   drift detected              -> RatioReseed
    //   otherwise                   -> WarmStart
    if (!m_havePrevSnapshot)   return SeedMode::ColdStart;
    if (m_prevWasProvisional)  return SeedMode::ProvisionalResume;
    if (driftDetected)         return SeedMode::RatioReseed;
    return SeedMode::WarmStart;
}

// ---------------------------------------------------------------------------
// Cycle lifecycle
// ---------------------------------------------------------------------------
bool DTStreamingMCMC::initializeCycle(SeedMode mode, QString &errorMessage)
{
    const unsigned int np = MCMC_Settings.number_of_parameters;
    const unsigned int nc = MCMC_Settings.number_of_chains;

    if (np == 0)
    {
        errorMessage = "no parameters registered on the System";
        return false;
    }
    if (nc == 0)
    {
        errorMessage = "number_of_chains is 0 -- was SetParameters() "
                       "called with the model's 'MCMC' Settings object?";
        return false;
    }

    // Fresh ensemble; per-cycle bookkeeping reset.
    m_chains.assign(nc, DTChainState());
    m_sweepIndex       = 0;
    m_cullCount        = 0;
    m_dissolutionCount = 0;
    m_evaluationCount  = 0;

    // Chain-private pristine model copies, made SERIALLY here so the
    // parallel sweep never copy-constructs from a shared source. The
    // shared model (*Model == the prepared calibration System) is fully
    // configured at this point: observations pushed, window patched,
    // weather injected, estimation mode set.
    {
        DTDebugLog &dlog = DTDebugLog::instance();
        dlog.log(DTDebugLog::Category::MCMC,
                 QString("pristine copies: starting (%1 chains)").arg(nc));
        dlog.flush();
        m_chainModels.clear();
        m_chainModels.reserve(nc);
        for (unsigned int c = 0; c < nc; ++c)
        {
            m_chainModels.push_back(*Model);
            dlog.log(DTDebugLog::Category::MCMC,
                     QString("pristine copy %1/%2 done").arg(c + 1).arg(nc));
            dlog.flush();
        }
        dlog.log(DTDebugLog::Category::MCMC, "pristine copies: done");
        dlog.flush();
    }

    // Proposal scales: restore from the previous cycle's snapshot when
    // compatible, so adaptation accumulates across cycles (short cycles
    // may never hit an adaptation block on their own). Otherwise fall
    // back to the base class's range-based initialization.
    pertcoeff.resize(np);
    if (m_prevPertcoeff.size() == np)
    {
        pertcoeff = m_prevPertcoeff;
    }
    else
    {
        for (unsigned int i = 0; i < np; ++i)
        {
            if (parameter(i)->GetPriorDistribution() == "log-normal")
                pertcoeff[i] = MCMC_Settings.purturbation_factor *
                               (std::log(parameter(i)->GetRange().high) -
                                std::log(parameter(i)->GetRange().low));
            else
                pertcoeff[i] = MCMC_Settings.purturbation_factor *
                               (parameter(i)->GetRange().high -
                                parameter(i)->GetRange().low);
        }
    }

    // Position the chains per the seeding regime (Sec. 3.3 / 3.9).
    // Seeding routines only fill chain.params; the posterior evaluation
    // of every seed is centralized below so all regimes share one
    // parallel pass. (seedRatioWeighted, once implemented, is the
    // exception: it computes pi_t per candidate as a side effect and
    // will pre-fill logp to avoid a second solve.)
    {
        DTDebugLog &dlog = DTDebugLog::instance();
        if (dlog.enabled(DTDebugLog::Category::MCMC))
        {
            static const char *modeNames[] =
                { "cold_start", "warm_start", "ratio_reseed",
                 "provisional_resume" };
            dlog.log(DTDebugLog::Category::MCMC,
                     QString("initializeCycle: cycle=%1 mode=%2 chains=%3 "
                             "params=%4 pertcoeff=%5")
                         .arg(m_cycleIndex)
                         .arg(modeNames[static_cast<int>(mode)])
                         .arg(nc).arg(np)
                         .arg(m_prevPertcoeff.size() == np
                                  ? "carried" : "range-init"));
        }
    }

    DTDebugLog::instance().log(DTDebugLog::Category::MCMC, "seeding: dispatch");
    DTDebugLog::instance().flush();

    bool ok = false;
    switch (mode)
    {
    case SeedMode::ColdStart:         ok = seedColdStart(errorMessage);         break;
    case SeedMode::WarmStart:         ok = seedWarmStart(errorMessage);         break;
    case SeedMode::RatioReseed:       ok = seedRatioWeighted(errorMessage);     break;
    case SeedMode::ProvisionalResume: ok = seedFromCarriedStates(errorMessage); break;
    }
    if (!ok) return false;

    // Evaluate the CURRENT cycle's target at every seed. Even warm-start
    // seeds need this: the window and kernel weights have moved since the
    // logp values stored in the previous snapshot were computed.
    DTDebugLog::instance().log(DTDebugLog::Category::MCMC,
                               QString("seed evaluation: starting parallel pass (chains=%1, threads=%2)")
                                   .arg(nc).arg(MCMC_Settings.numberOfThreads));
    DTDebugLog::instance().flush();
#ifndef NO_OPENMP
    omp_set_num_threads(MCMC_Settings.numberOfThreads);
#endif
#pragma omp parallel for
    for (int c = 0; c < static_cast<int>(nc); ++c)
    {
        DTChainState &chain = m_chains[c];
        if (std::isfinite(chain.logp) && chain.logp > -1e299)
            continue;   // pre-filled by the seeding routine

        double lp = posteriorLocal(c, chain.params);
        // NaN would poison the acceptance comparison in stepChain
        // (never accepts); map to the finite sentinel so the chain
        // self-heals on its first finite proposal instead.
        if (!std::isfinite(lp)) lp = -1e300;
        chain.logp = lp;
    }
    m_evaluationCount += nc;

    // Seed each trace with the initial level; phase starts Climbing.
    // The seed entry counts as "accepted" so the stagnation guard's
    // denominator and numerator start consistent.
    for (DTChainState &chain : m_chains)
    {
        chain.trace.push_back(chain.logp);
        chain.acceptTrace.push_back(1);
    }

    {
        DTDebugLog &dlog = DTDebugLog::instance();
        if (dlog.enabled(DTDebugLog::Category::MCMC))
        {
            double lo = 1e300, hi = -1e300;
            int dead = 0;
            for (const DTChainState &chain : m_chains)
            {
                lo = std::min(lo, chain.logp);
                hi = std::max(hi, chain.logp);
                if (chain.logp <= -1e299) ++dead;
            }
            dlog.log(DTDebugLog::Category::MCMC,
                     QString("seeds evaluated: logp range [%1, %2], "
                             "failed solves=%3/%4")
                         .arg(lo).arg(hi).arg(dead).arg(nc));
        }
        if (dlog.enabled(DTDebugLog::Category::MCMCTrace))
            for (size_t c = 0; c < m_chains.size(); ++c)
                dlog.log(DTDebugLog::Category::MCMCTrace,
                         QString("seed chain=%1 logp=%2")
                             .arg(c).arg(m_chains[c].logp));
    }

    return true;
}

DTCycleResult DTStreamingMCMC::runCycle(const QDateTime &deadline)
{
    DTCycleResult result;

    const int nc = static_cast<int>(m_chains.size());
    if (nc == 0)
    {
        std::cerr << "[StreamMCMC] runCycle called before initializeCycle "
                     "-- publishing empty provisional result\n";
        return result;
    }

    // Per-CHAIN (not per-thread) RNGs, seeded serially from m_seedRng:
    // chain c's stream is identical regardless of how OpenMP schedules
    // the sweep, so runs are reproducible for a fixed master seed.
    std::vector<std::mt19937_64> rngs(nc);
    for (int c = 0; c < nc; ++c)
        rngs[c].seed(m_seedRng());

#ifndef NO_OPENMP
    omp_set_num_threads(MCMC_Settings.numberOfThreads);
#endif

    // ---- wall-clock-bounded sampling loop (Sec. 3.8, Alg. 1 12-23) ----
    // Deadline checked between sweeps (every stepsPerClockCheck sweeps);
    // an in-flight sweep cannot be aborted, so the cycle can overrun by
    // up to one forward-solve duration -- the caller's Tcal margin
    // absorbs this. If the deadline is already past at entry, the body
    // never runs and the zero-sweep publication path below still yields
    // a well-formed provisional result.
    DTDebugLog &dlog = DTDebugLog::instance();
    if (dlog.enabled(DTDebugLog::Category::MCMC))
        dlog.log(DTDebugLog::Category::MCMC,
                 QString("runCycle: cycle=%1 chains=%2 threads=%3 "
                         "deadline=%4")
                     .arg(m_cycleIndex).arg(nc)
                     .arg(MCMC_Settings.numberOfThreads)
                     .arg(deadline.toString(Qt::ISODateWithMs)));

    const int clockStride = std::max(1, streamSettings.stepsPerClockCheck);
    while (true)
    {
        if (m_sweepIndex % clockStride == 0 &&
            QDateTime::currentDateTimeUtc() >= deadline)
            break;

        // Parallel sweep: one MH step + classification per chain.
        // stepChain/classifyChain touch only chain c, so the sweep is
        // race-free; anything requiring an ensemble-wide read happens
        // in the serial section below.
#pragma omp parallel for
        for (int c = 0; c < nc; ++c)
        {
            stepChain(c, rngs[c]);
            classifyChain(c, m_sweepIndex);
        }

        // Serial section: interventions (Sec. 3.5, 3.7). These read the
        // whole ensemble (quorum, plateaued level, donor selection) and
        // cloneChain copies another chain's state, so they must run
        // between sweeps, never inside the parallel loop. Cost is O(Nc)
        // comparisons -- negligible next to Nc forward solves.
        // (Phase 2: currently both checks are stubs returning false.)
        for (int c = 0; c < nc; ++c)
        {
            if (dissolutionCheck(c, rngs[c]))
            {
                ++m_dissolutionCount;
                continue;   // freshly cloned; leave it alone this sweep
            }
            if (cullCheck(c, rngs[c]))
                ++m_cullCount;
        }

        ++m_sweepIndex;

        if (m_sweepIndex % std::max(1, streamSettings.adaptationBlock) == 0)
        {
            adaptProposalScale();

            // Heartbeat at adaptation cadence: where is the ensemble?
            if (dlog.enabled(DTDebugLog::Category::MCMC))
            {
                std::vector<double> lps;
                lps.reserve(m_chains.size());
                for (const DTChainState &chain : m_chains)
                    lps.push_back(chain.logp);
                std::nth_element(lps.begin(),
                                 lps.begin() + lps.size() / 2, lps.end());
                dlog.log(DTDebugLog::Category::MCMC,
                         QString("heartbeat: sweep=%1 plateaued=%2 "
                                 "median_logp=%3 evals=%4")
                             .arg(m_sweepIndex)
                             .arg(plateauedFraction())
                             .arg(lps[lps.size() / 2])
                             .arg(m_evaluationCount));
            }
        }
    }

    // ---- publication (Sec. 3.9, Alg. 1 24-29) ----
    result.converged         = quorumHolds();
    result.plateauedFraction = plateauedFraction();
    result.totalSweeps       = m_sweepIndex;
    result.totalEvaluations  = m_evaluationCount;
    result.cullCount         = m_cullCount;
    result.dissolutionCount  = m_dissolutionCount;

    // Cycle acceptance rate: block totals plus the still-open window.
    {
        qint64 acc = m_acceptedTotal, prop = m_proposedTotal;
        for (const DTChainState &chain : m_chains)
        {
            acc  += chain.acceptedInWindow;
            prop += chain.proposedInWindow;
        }
        result.acceptanceRate =
            (prop > 0) ? static_cast<double>(acc) / static_cast<double>(prop)
                       : 0.0;
    }

    // Chain states always published: the provisional payload when
    // !converged (carried states, Sec. 3.9), and a diagnostic record
    // either way.
    result.chainParams.reserve(nc);
    result.chainLogp.reserve(nc);
    for (const DTChainState &chain : m_chains)
    {
        result.chainParams.push_back(chain.params);
        result.chainLogp.push_back(chain.logp);
    }

    if (result.converged)
    {
        assemblePool(result);
        result.summary             = computeSummary(result.pooledSamples);
        result.effectiveSampleSize = effectiveSampleSize(result.pooledSamples);
    }
    // Posterior summaries are computed ONLY from a converged pool; a
    // provisional result carries no summary -- mid-transit dispersion is
    // not posterior width (Sec. 3.9).

    result.pointEstimate = posteriorMAP(result);

    ++m_cycleIndex;
    logCycleDiagnostics(result);

    if (dlog.enabled(DTDebugLog::Category::MCMC))
        dlog.log(DTDebugLog::Category::MCMC,
                 QString("cycle %1 published: %2 plateaued=%3 pool=%4 "
                         "accept=%5 sweeps=%6 evals=%7")
                     .arg(m_cycleIndex)
                     .arg(result.converged ? "FULL" : "PROVISIONAL")
                     .arg(result.plateauedFraction)
                     .arg(result.pooledSamples.size())
                     .arg(result.acceptanceRate)
                     .arg(result.totalSweeps)
                     .arg(result.totalEvaluations));
    DTDebugLog::instance().flush();   // cycle boundary: make the log durable

    return result;
}

// ---------------------------------------------------------------------------
// Point estimates (Sec. 3.9)
// ---------------------------------------------------------------------------
std::vector<double> DTStreamingMCMC::posteriorMean(const DTCycleResult &result) const
{
    // Converged: mean of the pooled posterior. Provisional: mean of the
    // mid-transit chain states -- a point product only, never dispersion
    // (Sec. 3.9).
    const std::vector<std::vector<double>> &src =
        (result.converged && !result.pooledSamples.empty())
            ? result.pooledSamples
            : result.chainParams;
    if (src.empty()) return {};

    const size_t np = src.front().size();
    std::vector<double> mean(np, 0.0);
    for (const std::vector<double> &s : src)
        for (size_t i = 0; i < np; ++i)
            mean[i] += s[i];
    for (size_t i = 0; i < np; ++i)
        mean[i] /= static_cast<double>(src.size());
    return mean;
}

std::vector<double> DTStreamingMCMC::posteriorMAP(const DTCycleResult &result) const
{
    // Converged: highest-logp retained sample (sampled MAP -- the direct
    // analogue of the GA's returned mode). Provisional: highest-posterior
    // carried chain state (Sec. 3.9).
    const bool usePool = (result.converged && !result.pooledSamples.empty());
    const std::vector<std::vector<double>> &src =
        usePool ? result.pooledSamples : result.chainParams;
    const std::vector<double> &lp =
        usePool ? result.pooledLogp : result.chainLogp;
    if (src.empty() || lp.size() != src.size()) return {};

    size_t best = 0;
    for (size_t k = 1; k < lp.size(); ++k)
        if (lp[k] > lp[best]) best = k;
    return src[best];
}

// ---------------------------------------------------------------------------
// Seeding (Sec. 3.3, 3.9)
// ---------------------------------------------------------------------------
bool DTStreamingMCMC::seedColdStart(QString &errorMessage)
{
    Q_UNUSED(errorMessage);

    // Phase 1 interim: plain prior draws per chain (the semantics of
    // CMCMC::initialize(random=true)), so the skeleton runs end-to-end.
    //
    // TODO (Phase 2) -- importance-resampled initialization (Sec. 3.3):
    //   - K = coldStartMultiplier * Nc candidates via drawFromRange()
    //   - evaluate posterior at each (embarrassingly parallel; one
    //     forward solve per candidate), pre-filling logp for the chosen
    //     seeds so initializeCycle skips re-evaluation
    //   - IMPORTANCE WEIGHTS: the spec's "weight prop. to exp[J]" assumes
    //     candidates drawn from the prior. drawFromRange() draws
    //     range-uniform (log-uniform for log-normal priors), NOT the
    //     prior density, so the correct log-weight for this proposal is
    //     log-prior + J -- which is exactly the value posterior() returns.
    //     Resample Nc seeds PROPORTIONAL to exp(posterior value) via
    //     log-sum-exp normalization -- never top-Nc (Sec. 3.3)
    //   - explorerFraction of chains overwritten with raw prior draws,
    //     flagged chain.explorer = true
    for (DTChainState &chain : m_chains)
        chain.params = drawFromRange(m_seedRng);

    return true;
}

bool DTStreamingMCMC::seedWarmStart(QString &errorMessage)
{
    // Uniform, UNWEIGHTED draws with replacement from the previous
    // cycle's pooled posterior (Sec. 3.3). The pooled samples are already
    // (approximately) target-distributed; weighting them by posterior
    // value would apply the density twice, concentrate seeds toward the
    // mode, and under-disperse the ensemble -- a bias that compounds
    // across cycles because each cycle's pool seeds the next.
    if (!m_havePrevSnapshot || m_prevSamples.empty())
    {
        errorMessage = "warm start requested but no previous pool is "
                       "loaded (loadPosteriorSnapshot not called or "
                       "snapshot empty)";
        return false;
    }
    const unsigned int np = MCMC_Settings.number_of_parameters;
    for (const std::vector<double> &s : m_prevSamples)
    {
        if (s.size() != np)
        {
            errorMessage = QString("previous pool has %1 parameters, "
                                   "model has %2 -- snapshot/model mismatch")
                               .arg(s.size()).arg(np);
            return false;
        }
    }

    std::uniform_int_distribution<size_t> pick(0, m_prevSamples.size() - 1);
    for (DTChainState &chain : m_chains)
        chain.params = m_prevSamples[pick(m_seedRng)];

    // logp deliberately NOT copied from m_prevLogp: those values were
    // computed against the previous cycle's window and kernel weights.
    // initializeCycle re-evaluates every seed against the current target.
    return true;
}

bool DTStreamingMCMC::seedRatioWeighted(QString &errorMessage)
{
    // TODO (Phase 3):
    //   - evaluate pi_t at every sample of m_prevSamples (one forward solve
    //     each); importance weight w_k = exp(logpi_t(k) - m_prevLogp[k]),
    //     normalized via log-sum-exp
    //   - resample Nc seeds proportional to w_k (SMC reweighting step)
    //   - reuse the freshly computed logpi_t as each seeded chain's logp
    //     (no second evaluation needed)
    errorMessage = "DTStreamingMCMC::seedRatioWeighted not implemented";
    return false;
}

bool DTStreamingMCMC::seedFromCarriedStates(QString &errorMessage)
{
    // Provisional resume (Sec. 3.9): each chain picks up exactly where it
    // left off, so migration toward a relocated target accumulates across
    // cycles instead of restarting at each. A cycle is NOT expected to
    // converge; it is expected to leave the ensemble better located than
    // it found it, and this function is what banks that improvement.
    if (!m_havePrevSnapshot || m_prevSamples.empty())
    {
        errorMessage = "provisional resume requested but no carried chain "
                       "states are loaded";
        return false;
    }
    const unsigned int np = MCMC_Settings.number_of_parameters;
    for (const std::vector<double> &s : m_prevSamples)
    {
        if (s.size() != np)
        {
            errorMessage = QString("carried chain state has %1 parameters, "
                                   "model has %2 -- snapshot/model mismatch")
                               .arg(s.size()).arg(np);
            return false;
        }
    }

    const size_t nc = m_chains.size();
    if (m_prevSamples.size() == nc)
    {
        // Normal case: chain count unchanged; exact resumption.
        for (size_t c = 0; c < nc; ++c)
            m_chains[c].params = m_prevSamples[c];
    }
    else
    {
        // Chain count changed between cycles (config edit): resume by
        // uniform draw from the carried states -- inexact but preserves
        // the ensemble's location, which is what the resume is for.
        std::cerr << "[StreamMCMC] chain count changed ("
                  << m_prevSamples.size() << " carried, " << nc
                  << " configured) -- resuming by uniform draw from "
                     "carried states\n";
        std::uniform_int_distribution<size_t> pick(0, m_prevSamples.size() - 1);
        for (DTChainState &chain : m_chains)
            chain.params = m_prevSamples[pick(m_seedRng)];
    }

    // logp deliberately NOT restored from the snapshot: the target has
    // moved since it was written (new observations, kernel re-weighting).
    // initializeCycle re-evaluates every seed against the current target,
    // and each chain must re-plateau under that target before its samples
    // pool (Sec. 3.4) -- position is carried, convergence status is not.
    return true;
}

std::vector<double> DTStreamingMCMC::drawFromRange(std::mt19937_64 &rng)
{
    // Overdispersed range draw, NOT a prior draw (see header note).
    // Mirrors CMCMC::initialize(random=true): uniform over the range for
    // normal/uniform priors, uniform in log space for log-normal priors.
    const unsigned int np = MCMC_Settings.number_of_parameters;
    std::vector<double> x(np);
    std::uniform_real_distribution<double> U(0.0, 1.0);

    for (unsigned int i = 0; i < np; ++i)
    {
        const double lo = parameter(i)->GetRange().low;
        const double hi = parameter(i)->GetRange().high;
        if (parameter(i)->GetPriorDistribution() == "log-normal")
            x[i] = std::exp(std::log(lo) + (std::log(hi) - std::log(lo)) * U(rng));
        else
            x[i] = lo + (hi - lo) * U(rng);
    }
    return x;
}

// ---------------------------------------------------------------------------
// Per-chain mechanics
// ---------------------------------------------------------------------------
double DTStreamingMCMC::posteriorLocal(int c, const std::vector<double> &par)
{
    // Copy-solve-discard against the chain's OWN pristine model. Same
    // semantics and value as CMCMC::posterior (log-prior plus the
    // negated engine objective, i.e. the kernel-weighted log-likelihood)
    // -- but the copy source is chain-private, so concurrent sweeps
    // never read the same System, and the base's per-evaluation
    // detail-file omp critical is out of the hot path.
    System work = m_chainModels[c];
    work.SetSilent(true);
    work.SetRecordResults(false);
    work.SetNumThreads(1);

    double sum = 0;
    for (int i = 0; i < MCMC_Settings.number_of_parameters; ++i)
    {
        work.SetParameterValue(i, par[i]);
        sum += parameter(i)->CalcLogPriorProbability(par[i]);
    }
    work.ApplyParameters();
    work.Solve();
    return sum - work.GetObjectiveFunctionValue();
}

// ---------------------------------------------------------------------------
bool DTStreamingMCMC::stepChain(int c, std::mt19937_64 &rng)
{
    DTChainState &chain = m_chains[c];

    double logJ = 0.0;
    std::vector<double> X = proposeFrom(chain.params, rng, logJ);

    // Pure log-posterior: log-prior + kernel-weighted log-likelihood.
    // One forward solve; the dominant cost of the step.
    const double logp0 = posteriorLocal(c, X);

#pragma omp atomic
    ++m_evaluationCount;

    // Within-chain Metropolis-Hastings acceptance (Sec. 3.1), in log
    // space to avoid exp() overflow. Non-finite proposals always reject.
    // Unlike CMCMC::step, the log-normal Hastings correction enters the
    // acceptance ratio here and is NOT folded into the stored logp, so
    // chain.logp / trace / sampleLogp are honest log-posteriors (the
    // classifier, cull margin, MAP, and ratio reseed all rely on this).
    bool accepted = false;
    if (std::isfinite(logp0))
    {
        const double logAccept = (logp0 + logJ) - chain.logp;
        if (logAccept >= 0.0)
        {
            accepted = true;
        }
        else
        {
            std::uniform_real_distribution<double> U(0.0, 1.0);
            accepted = (std::log(U(rng)) < logAccept);
        }
    }

    if (accepted)
    {
        chain.params       = std::move(X);
        chain.logp         = logp0;
        chain.stuckCounter = 0;
        ++chain.acceptedInWindow;
    }
    else
    {
        ++chain.stuckCounter;
    }
    ++chain.proposedInWindow;

    {
        DTDebugLog &dlog = DTDebugLog::instance();
        if (dlog.enabled(DTDebugLog::Category::MCMCTrace))
            dlog.log(DTDebugLog::Category::MCMCTrace,
                     QString("step chain=%1 %2 logp_prop=%3 logJ=%4 "
                             "logp_cur=%5 stuck=%6")
                         .arg(c)
                         .arg(accepted ? "ACC" : "rej")
                         .arg(logp0).arg(logJ)
                         .arg(chain.logp).arg(chain.stuckCounter));
    }

    // Classifier input: one trace point per step, accepted or not
    // (a rejected step re-records the current level, exactly as the
    // base driver re-records Params[k] on rejection).
    chain.trace.push_back(chain.logp);
    chain.acceptTrace.push_back(accepted ? 1 : 0);
    while (static_cast<int>(chain.trace.size()) >
           streamSettings.plateauWindow)
        chain.trace.pop_front();
    while (chain.acceptTrace.size() > chain.trace.size())
        chain.acceptTrace.pop_front();

    // Burn-in exclusion at retention time (Sec. 3.4): only a plateaued
    // chain's states enter the retained store. Rejected steps repeat the
    // current state -- required for correct MH sample density.
    if (chain.phase == ChainPhase::Plateaued)
    {
        chain.samples.push_back(chain.params);
        chain.sampleLogp.push_back(chain.logp);
    }

    return accepted;
}

std::vector<double> DTStreamingMCMC::proposeFrom(const std::vector<double> &x,
                                                 std::mt19937_64 &rng,
                                                 double &logJacobian)
{
    // Mirrors CMCMC::purturb(): additive Gaussian for normal/uniform
    // priors, multiplicative log-normal for log-normal priors -- but with
    // a caller-supplied RNG (thread-safe) and the Hastings correction
    // returned explicitly instead of being folded into the stored logp.
    const unsigned int np = MCMC_Settings.number_of_parameters;
    std::vector<double> X(np);
    std::normal_distribution<double> N(0.0, 1.0);

    logJacobian = 0.0;
    for (unsigned int i = 0; i < np; ++i)
    {
        if (parameter(i)->GetPriorDistribution() == "log-normal")
        {
            X[i] = x[i] * std::exp(pertcoeff[i] * N(rng));
            // q(x|x') / q(x'|x) = x'_i / x_i for the multiplicative
            // log-normal random walk.
            logJacobian += std::log(X[i]) - std::log(x[i]);
        }
        else
        {
            X[i] = x[i] + pertcoeff[i] * N(rng);
        }
    }
    return X;
}

// ---------------------------------------------------------------------------
// Plateau classifier and quorum (Sec. 3.2)
// ---------------------------------------------------------------------------
void DTStreamingMCMC::classifyChain(int c, qint64 sweepIndex)
{
    DTChainState &chain = m_chains[c];

    const int n = static_cast<int>(chain.trace.size());

    // Too little history to call a trend either way (Sec. 3.2).
    if (n < streamSettings.minStepsBeforeClassify)
    {
        chain.phase = ChainPhase::Climbing;
        chain.plateauOnsetStep = -1;
        return;
    }

    // A chain whose window still contains non-finite or sentinel values
    // (never successfully evaluated, or parked in an infeasible region)
    // is not sampling anything classifiable.
    for (const double v : chain.trace)
    {
        if (!std::isfinite(v) || v <= -1e299)
        {
            chain.phase = ChainPhase::Climbing;
            chain.plateauOnsetStep = -1;
            return;
        }
    }

    // Least-squares slope of logp against step index over the window.
    // With k = 0..n-1: b = sum((k - kbar)(y - ybar)) / sum((k - kbar)^2).
    const double kbar = 0.5 * (n - 1);
    double ybar = 0.0;
    for (const double v : chain.trace) ybar += v;
    ybar /= n;

    double sxy = 0.0, sxx = 0.0;
    {
        int k = 0;
        for (const double v : chain.trace)
        {
            const double dk = k - kbar;
            sxy += dk * (v - ybar);
            sxx += dk * dk;
            ++k;
        }
    }
    const double b = sxy / sxx;

    // Scatter of the residuals about the fitted line (n-2 dof).
    double ss = 0.0;
    {
        int k = 0;
        for (const double v : chain.trace)
        {
            const double r = (v - ybar) - b * (k - kbar);
            ss += r * r;
            ++k;
        }
    }
    const double s = std::sqrt(ss / std::max(n - 2, 1));

    // Stagnation guard: a window carried by too few ACCEPTED moves is a
    // stuck chain, not a plateau -- its trace is flat because nothing
    // moved, and treating that as convergence is what produced the
    // spurious "PLATEAUED at sweep 8, scatter=1e-13" events. Requires a
    // minimum fraction of accepts in the window before a plateau can be
    // declared. (Trajectory-plus-motion, still never level-based.)
    int acceptedInTrace = 0;
    for (const char a : chain.acceptTrace) acceptedInTrace += a;
    const bool moving =
        acceptedInTrace >=
        std::max(1, static_cast<int>(streamSettings.minAcceptedFraction * n));

    // Plateaued iff the chain is moving AND the total trend across the
    // window is small relative to the within-window scatter (Sec. 3.2).
    // TRAJECTORY-based: the chain's absolute level never enters, so a
    // chain flat-but-moving at a low level (secondary mode / low-density
    // ridge) still classifies plateaued.
    //
    // Hysteresis: DECLARE below plateauSlopeThreshold; once plateaued,
    // REVERT only above plateauSlopeThreshold * plateauRevertFactor.
    // Window statistics are noisy enough that a single threshold flaps.
    const double totalTrend = std::fabs(b) * (n - 1);
    const double declareAt  = streamSettings.plateauSlopeThreshold;
    const double revertAt   = declareAt * std::max(1.0, streamSettings.plateauRevertFactor);

    bool plateaued;
    if (!moving)
    {
        plateaued = false;   // stagnation: dissolution's territory (Phase 2)
    }
    else if (s > 0.0)
    {
        const double ratio = totalTrend / s;
        plateaued = (chain.phase == ChainPhase::Plateaued)
                        ? (ratio < revertAt)     // stickier once declared
                        : (ratio < declareAt);
    }
    else
    {
        // Zero scatter with accepted moves in-window is effectively
        // impossible for a continuous target; classify conservatively.
        plateaued = false;
    }

    if (plateaued)
    {
        if (chain.phase == ChainPhase::Climbing)
        {
            chain.plateauOnsetStep = static_cast<int>(sweepIndex);
            DTDebugLog &dlog = DTDebugLog::instance();
            if (dlog.enabled(DTDebugLog::Category::MCMC))
                dlog.log(DTDebugLog::Category::MCMC,
                         QString("chain %1 PLATEAUED at sweep %2 "
                                 "(logp=%3, trend=%4, scatter=%5)")
                             .arg(c).arg(sweepIndex)
                             .arg(chain.logp).arg(totalTrend).arg(s));
        }
        chain.phase = ChainPhase::Plateaued;
    }
    else
    {
        if (chain.phase == ChainPhase::Plateaued)
        {
            DTDebugLog &dlog = DTDebugLog::instance();
            if (dlog.enabled(DTDebugLog::Category::MCMC))
                dlog.log(DTDebugLog::Category::MCMC,
                         QString("chain %1 REVERTED to climbing at sweep %2 "
                                 "(logp=%3, trend=%4, scatter=%5) — "
                                 "target moved under the chain?")
                             .arg(c).arg(sweepIndex)
                             .arg(chain.logp).arg(totalTrend).arg(s));
        }
        // Includes Plateaued -> Climbing reversion (target moved under
        // the chain). Already-retained samples are kept -- they were drawn
        // while genuinely plateaued -- but retention stops until the chain
        // re-plateaus (enforced by the phase check in stepChain).
        chain.phase = ChainPhase::Climbing;
        chain.plateauOnsetStep = -1;
    }
}

double DTStreamingMCMC::plateauedFraction() const
{
    if (m_chains.empty()) return 0.0;
    int count = 0;
    for (const DTChainState &chain : m_chains)
        if (chain.phase == ChainPhase::Plateaued) ++count;
    return static_cast<double>(count) / static_cast<double>(m_chains.size());
}

bool DTStreamingMCMC::quorumHolds() const
{
    if (m_chains.empty()) return false;
    return plateauedFraction() >= streamSettings.quorumFraction;
}

double DTStreamingMCMC::plateauedEnsembleLevel() const
{
    // TODO (Phase 2): median of chain.logp over plateaued chains.
    // Median, not mean — a single plateaued chain in a deep secondary mode
    // must not drag the cull reference level down.
    return -1e300;
}

// ---------------------------------------------------------------------------
// Interventions (Sec. 3.5, 3.7) — pre-plateau phase only
// ---------------------------------------------------------------------------
bool DTStreamingMCMC::cullCheck(int c, std::mt19937_64 &rng)
{
    // TODO (Phase 2):
    //   Guards, in order (Sec. 3.5):
    //     1. streamSettings.cullEnabled
    //     2. quorumHolds()            — no stable reference level otherwise;
    //                                   also suspends the cull during
    //                                   post-drift migration (Sec. 3.10)
    //     3. chain c is Climbing      — plateaued chains are NEVER culled
    //     4. chain.logp < plateauedEnsembleLevel() - cullMarginLogp
    //   On fire: cloneChain(randomPlateauedChain(rng), c); ++m_cullCount.
    //   Uniformly RANDOM donor, never the best chain (Sec. 3.5).
    Q_UNUSED(c);
    Q_UNUSED(rng);
    return false;
}

bool DTStreamingMCMC::dissolutionCheck(int c, std::mt19937_64 &rng)
{
    // TODO (Phase 2):
    //   Mechanical health only — posterior level never enters (Sec. 3.7):
    //     - stuckCounter > stuckThreshold, OR
    //     - acceptance over the window < minAcceptanceRate with a full
    //       window of proposals, OR
    //     - !isfinite(chain.logp)
    //   On fire: cloneChain(randomHealthyChain(rng), c) — plateaued donor
    //   if quorum exists, any moving chain otherwise; ++m_dissolutionCount.
    Q_UNUSED(c);
    Q_UNUSED(rng);
    return false;
}

void DTStreamingMCMC::cloneChain(int src, int dst)
{
    // TODO (Phase 2):
    //   dst.params/logp <- src's current position/logp
    //   dst.trace cleared and reseeded with the cloned logp
    //   dst.phase = Climbing, plateauOnsetStep = -1  (must re-plateau
    //     before its samples pool again, Sec. 3.5)
    //   dst health counters reset; ++dst.reseedCount
    //   dst retained samples are KEPT — they were drawn while genuinely
    //   plateaued in this cycle... [decide: for a culled climbing chain the
    //   store is empty anyway; for a dissolved formerly-plateaued chain,
    //   keeping them is correct — they are valid posterior draws]
    Q_UNUSED(src);
    Q_UNUSED(dst);
}

int DTStreamingMCMC::randomPlateauedChain(std::mt19937_64 &rng) const
{
    // TODO (Phase 2): uniform over indices with phase == Plateaued; -1 if none.
    Q_UNUSED(rng);
    return -1;
}

int DTStreamingMCMC::randomHealthyChain(std::mt19937_64 &rng) const
{
    // TODO (Phase 2): uniform over chains passing the mechanical-health
    // test; prefer plateaued chains when any exist; -1 if none.
    Q_UNUSED(rng);
    return -1;
}

// ---------------------------------------------------------------------------
// Adaptation, pooling, diagnostics
// ---------------------------------------------------------------------------
void DTStreamingMCMC::adaptProposalScale()
{
    // Pooled acceptance over the just-closed block, from the per-chain
    // window counters (the base class's global rule, re-driven).
    qint64 acc = 0, prop = 0;
    for (DTChainState &chain : m_chains)
    {
        acc  += chain.acceptedInWindow;
        prop += chain.proposedInWindow;
        chain.acceptedInWindow = 0;
        chain.proposedInWindow = 0;
    }
    m_acceptedTotal += acc;
    m_proposedTotal += prop;

    if (prop == 0) return;

    const double blockRate =
        static_cast<double>(acc) / static_cast<double>(prop);
    {
        DTDebugLog &dlog = DTDebugLog::instance();
        if (dlog.enabled(DTDebugLog::Category::MCMC))
            dlog.log(DTDebugLog::Category::MCMC,
                     QString("adapt block: rate=%1 target=%2 %3 "
                             "(pertcoeff[0]=%4)")
                         .arg(blockRate)
                         .arg(MCMC_Settings.acceptance_rate)
                         .arg(quorumHolds()
                                  ? "FROZEN (quorum holds)"
                                  : (blockRate > MCMC_Settings.acceptance_rate
                                         ? "-> enlarge steps"
                                         : "-> shrink steps"))
                         .arg(pertcoeff.empty() ? 0.0 : pertcoeff[0]));
    }

    // Freeze the proposal kernel once a quorum has plateaued: continued
    // adaptation while samples are being retained violates (strictly)
    // the invariance the pooled posterior rests on. Before quorum, the
    // retained pool is small-to-empty and adaptation is cheap insurance;
    // after quorum, post-plateau samples are drawn under a fixed kernel.
    // Counter bookkeeping above still runs so the cycle acceptance rate
    // stays accurate.
    if (quorumHolds()) return;

    const double rate = blockRate;
    if (rate > MCMC_Settings.acceptance_rate)
    {
        // Accepting too often: proposals too timid -- enlarge steps.
        for (double &p : pertcoeff) p /= MCMC_Settings.purt_change_scale;
    }
    else
    {
        for (double &p : pertcoeff) p *= MCMC_Settings.purt_change_scale;
    }
}

void DTStreamingMCMC::assemblePool(DTCycleResult &result) const
{
    // Equally weighted concatenation of each chain's retained
    // (post-plateau) samples (Alg. 1 line 25). No per-sample importance
    // weights: the posterior's shape is carried by sample density alone.
    // Burn-in exclusion already happened at retention time -- stepChain
    // stores a sample only while the chain is classified Plateaued.
    size_t total = 0;
    for (const DTChainState &chain : m_chains)
        total += chain.samples.size();

    result.pooledSamples.clear();
    result.pooledLogp.clear();
    result.pooledSamples.reserve(total);
    result.pooledLogp.reserve(total);

    for (const DTChainState &chain : m_chains)
    {
        result.pooledSamples.insert(result.pooledSamples.end(),
                                    chain.samples.begin(),
                                    chain.samples.end());
        result.pooledLogp.insert(result.pooledLogp.end(),
                                 chain.sampleLogp.begin(),
                                 chain.sampleLogp.end());
    }
}

DTPosteriorSummary DTStreamingMCMC::computeSummary(
    const std::vector<std::vector<double>> &samples) const
{
    DTPosteriorSummary summary;
    const size_t n = samples.size();
    if (n == 0) return summary;
    const size_t np = samples.front().size();

    // Means.
    summary.mean.assign(np, 0.0);
    for (const std::vector<double> &s : samples)
        for (size_t i = 0; i < np; ++i)
            summary.mean[i] += s[i];
    for (size_t i = 0; i < np; ++i)
        summary.mean[i] /= static_cast<double>(n);

    // Joint covariance (sample covariance, n-1 denominator) and stdev
    // from its diagonal. This is the matrix that exposes the
    // n_eng--n_nat compensation structure directly.
    summary.covariance.assign(np, std::vector<double>(np, 0.0));
    for (const std::vector<double> &s : samples)
        for (size_t i = 0; i < np; ++i)
        {
            const double di = s[i] - summary.mean[i];
            for (size_t j = i; j < np; ++j)
                summary.covariance[i][j] += di * (s[j] - summary.mean[j]);
        }
    const double denom = (n > 1) ? static_cast<double>(n - 1) : 1.0;
    summary.stdev.assign(np, 0.0);
    for (size_t i = 0; i < np; ++i)
    {
        for (size_t j = i; j < np; ++j)
        {
            summary.covariance[i][j] /= denom;
            summary.covariance[j][i]  = summary.covariance[i][j];
        }
        summary.stdev[i] = std::sqrt(std::max(0.0, summary.covariance[i][i]));
    }

    // Percentiles per parameter: sorted marginal with linear
    // interpolation at rank q*(n-1).
    summary.p025.assign(np, 0.0);
    summary.p50.assign(np, 0.0);
    summary.p975.assign(np, 0.0);
    summary.p10.assign(np, 0.0);
    summary.p90.assign(np, 0.0);
    std::vector<double> column(n);
    const auto percentile = [&column, n](double q) -> double
    {
        const double rank = q * static_cast<double>(n - 1);
        const size_t k    = static_cast<size_t>(rank);
        if (k + 1 >= n) return column[n - 1];
        const double frac = rank - static_cast<double>(k);
        return column[k] + frac * (column[k + 1] - column[k]);
    };
    for (size_t i = 0; i < np; ++i)
    {
        for (size_t s = 0; s < n; ++s)
            column[s] = samples[s][i];
        std::sort(column.begin(), column.end());
        summary.p025[i] = percentile(0.025);
        summary.p50[i]  = percentile(0.50);
        summary.p975[i] = percentile(0.975);
        summary.p10[i]  = percentile(0.10);
        summary.p90[i]  = percentile(0.90);
    }

    return summary;
}

double DTStreamingMCMC::effectiveSampleSize(
    const std::vector<std::vector<double>> &samples) const
{
    // TODO (Phase 4):
    //   Per-parameter ESS from the pooled draws via initial positive
    //   sequence of autocorrelations (Geyer); report min across parameters.
    //   Note the pool interleaves chains — compute per chain then sum,
    //   rather than on the concatenated stream.
    Q_UNUSED(samples);
    return 0.0;
}

void DTStreamingMCMC::logCycleDiagnostics(const DTCycleResult &result) const
{
    std::cout << "[StreamMCMC]"
              << " cycle="        << m_cycleIndex
              << " converged="    << (result.converged ? 1 : 0)
              << " plateaued="    << result.plateauedFraction
              << " pool="         << result.pooledSamples.size()
              << " ess="          << result.effectiveSampleSize
              << " accept="       << result.acceptanceRate
              << " culls="        << result.cullCount
              << " dissolutions=" << result.dissolutionCount
              << " sweeps="       << result.totalSweeps
              << " evals="        << result.totalEvaluations
              << "\n";
}
