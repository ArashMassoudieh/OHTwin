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
#include "TimeSeriesSet.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <set>

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
    m_prevPropCov.clear();
    m_prevPropCovWeight = 0.0;
    m_prevKappa         = 0.0;

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

    // Adaptive-covariance state. Carried on the same terms as pertcoeff:
    // the parameter-name audit above already guarantees the coordinate
    // system is unchanged, so a dimensionally valid matrix is meaningful.
    // Restored even when the sample payload turns out to be unusable --
    // the accumulated proposal shape is still valid.
    std::vector<std::vector<double>> pcov =
        fromJsonMatrix(root.value("proposal_covariance").toArray());
    const double pcovW = root.value("proposal_covariance_weight").toDouble(0.0);
    bool pcovValid = (pcov.size() == np) && (pcovW > 0.0);
    if (pcovValid)
    {
        for (const std::vector<double> &row : pcov)
        {
            if (row.size() != np) { pcovValid = false; break; }
        }
    }
    if (pcovValid)
    {
        m_prevPropCov       = std::move(pcov);
        m_prevPropCovWeight = pcovW;
    }
    const double kap = root.value("kappa").toDouble(0.0);
    if (kap > 0.0 && std::isfinite(kap))
        m_prevKappa = kap;

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

    // Adaptive-covariance state. This object is destroyed at the end of the
    // cycle, so the running covariance accumulates across cycles only by
    // round-tripping here. Written unconditionally (converged or not) --
    // the proposal shape is a sampler property, not a posterior claim.
    // accumulateProposalCovariance() has already folded in this cycle's pool
    // by the time the caller reaches here.
    if (!m_propCov.empty() && m_propCovWeight > 0.0)
    {
        root["proposal_covariance"]        = toJsonMatrix(m_propCov);
        root["proposal_covariance_weight"] = m_propCovWeight;
    }
    if (m_kappa > 0.0) root["kappa"] = m_kappa;

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
bool DTStreamingMCMC::produceRealizationCI(const DTCycleResult &result,
                                           const QString &outputPath,
                                           double tNow,
                                           QString &errorMessage)
{
    // Reads the reservoir captured DURING sampling -- no re-solves. The
    // reservoir holds up to realizationCount modeled-output sets, drawn
    // uniformly at random from this cycle's pooled post-plateau samples.
    DTDebugLog &dlog = DTDebugLog::instance();
    dlog.log(DTDebugLog::Category::MCMC,
             QString("produceRealizationCI: ENTRY converged=%1 reservoir.size=%2 "
                     "reservoir.seen=%3 pool=%4 tNow=%5")
                 .arg(result.converged)
                 .arg(m_outputReservoir.outputs.size())
                 .arg(m_outputReservoir.seen)
                 .arg(result.pooledSamples.size())
                 .arg(tNow));
    dlog.flush();

    // Provisional cycles allowed: publish whatever the reservoir captured.
    // The only hard requirement is a non-empty reservoir.
    const DTOutputReservoir &R = m_outputReservoir;
    if (R.outputs.empty())
    {
        dlog.log(DTDebugLog::Category::MCMC,
                 "produceRealizationCI: BAIL reservoir empty");
        dlog.flush();
        errorMessage = "output reservoir is empty (no plateaued samples captured this cycle)";
        return false;
    }
    // Number of observations from the first stored realization.
    const int nobs = static_cast<int>(R.outputs.front().size());
    const int nreal = static_cast<int>(R.outputs.size());

    // ONE combined band file for the whole cycle. Each calibration
    // observation contributes four named value columns (Mean + 2.5/50/97.5 %,
    // via GUI getpercentiles); the observation name is a COLUMN LABEL, never
    // part of the filename. This is deliberate: observation names such as
    // "Underdrain flow (m3/day)" contain '/', which the old per-observation
    // filename scheme fed straight into std::ofstream -- the OS read it as a
    // directory separator and the open silently failed, dropping that
    // observation's band entirely. Labels inside the file have no such issue.
    const std::vector<double> pcts = {0.025, 0.5, 0.975};
    const QString tag = QString::number(static_cast<qint64>(tNow));

    TimeSeriesSet<double> combined;
    int nBands = 0;
    QStringList emptyObs;
    for (int o = 0; o < nobs; ++o)
    {
        // Restrict to calibration targets. DTAssimilation pushes observed_data
        // into exactly the observations named in calibration_observations; a
        // non-empty observed series is the reliable in-model marker. Model
        // observations that are computed but not calibrated against (e.g.
        // Evaporation) carry no observed_data and are skipped here.
        auto *odq = observation(o)->Variable("observed_data");
        const bool isCalib = odq && odq->GetTimeSeries()
                             && odq->GetTimeSeries()->size() > 0;
        if (!isCalib)
            continue;

        TimeSeriesSet<double> ensemble;
        for (int r = 0; r < nreal; ++r)
            if (o < static_cast<int>(R.outputs[r].size())
                && R.outputs[r][o].size() > 0)
                ensemble.append(R.outputs[r][o], std::to_string(r));

        const std::string obsName = observation(o)->GetName();
        if (ensemble.empty())
        {
            // A calibration target with no reservoir data this cycle -- surface
            // it rather than silently omitting the column.
            emptyObs << QString::fromStdString(obsName);
            continue;
        }

        // getpercentiles -> series named "Mean","2.500000 %",... Qualify each
        // with the observation name so a single file disambiguates columns:
        //   "Underdrain flow (m3/day) | 2.500000 %"
        TimeSeriesSet<double> bracket = ensemble.getpercentiles(pcts);
        for (size_t k = 0; k < bracket.size(); ++k)
            combined.append(bracket[k], obsName + " | " + bracket[k].name());
        ++nBands;
    }

    if (combined.empty())
    {
        dlog.log(DTDebugLog::Category::MCMC,
                 QString("produceRealizationCI: BAIL no calibration observation "
                         "had reservoir data (empty: %1)").arg(emptyObs.join(", ")));
        dlog.flush();
        errorMessage = "no calibration observation had reservoir data this cycle";
        return false;
    }

    combined.write((outputPath.toStdString()
                    + "/realization_ci_cycle_" + tag.toStdString() + ".txt"));
    // Stable-name copy so the viewer fetches one fixed URL per cycle.
    combined.write((outputPath.toStdString() + "/realization_ci_latest.txt"));

    if (dlog.enabled(DTDebugLog::Category::MCMC))
        dlog.log(DTDebugLog::Category::MCMC,
                 QString("realization band: %1 calib obs x %2 realizations at "
                         "t_now=%3 (seen=%4)%5")
                     .arg(nBands).arg(nreal).arg(tNow).arg(R.seen)
                     .arg(emptyObs.isEmpty() ? QString()
                                             : QString(" [no data: %1]").arg(emptyObs.join(", "))));
    return true;
}

// ---------------------------------------------------------------------------
bool DTStreamingMCMC::writePosteriorDistribution(const DTCycleResult &result,
                                                 const QString &outputPath,
                                                 double tNow,
                                                 QString &errorMessage)
{
    // Provisional cycles allowed: the posterior histogram + CI table are
    // published from whatever pooled post-plateau samples this cycle
    // retained, certified or not (see runCycle's assemblePool comment).
    // The only hard requirement is a non-empty pool.
    if (result.pooledSamples.empty())
    {
        errorMessage = "posterior distribution requested with no pooled samples";
        return false;
    }
    const int N  = static_cast<int>(result.pooledSamples.size());
    const int np = static_cast<int>(result.pooledSamples.front().size());
    const QString tag = QString::number(static_cast<qint64>(tNow));

    // Adaptive bin count: sqrt(N), clamped to [10, 60] -- smooth without
    // over-resolving a few-hundred-sample cycle pool.
    const int nbins = std::min(60, std::max(10,
                                            static_cast<int>(std::lround(std::sqrt((double)N)))));

    // Per-parameter histogram via TimeSeries::distribution over pooled values.
    TimeSeriesSet<double> dists;
    for (int i = 0; i < np; ++i)
    {
        // Build a value series for parameter i (time index is a dummy
        // ordinal; distribution() only reads the values).
        TimeSeries<double> col;
        for (int s = 0; s < N; ++s)
            col.append(static_cast<double>(s), result.pooledSamples[s][i]);
        TimeSeries<double> hist = col.distribution(nbins, 0);
        dists.append(hist, parameter(i)->GetName());
    }
    dists.write((outputPath.toStdString()
                 + "/posterior_dist_cycle_" + tag.toStdString() + ".txt"));

    dists.write((outputPath.toStdString() + "/posterior_dist_latest.txt"));

    // CI table: name, mean, stdev, p2.5, p50, p97.5 (from computeSummary,
    // already run on the full pool for this FULL cycle).
    const DTPosteriorSummary &s = result.summary;
    {
        QString ci;
        ci += "parameter\tmean\tstdev\tp2.5\tp50\tp97.5\n";
        for (int i = 0; i < np; ++i)
            ci += QString("%1\t%2\t%3\t%4\t%5\t%6\n")
                      .arg(QString::fromStdString(parameter(i)->GetName()))
                      .arg(i < (int)s.mean.size() ? s.mean[i] : 0.0)
                      .arg(i < (int)s.stdev.size() ? s.stdev[i] : 0.0)
                      .arg(i < (int)s.p025.size() ? s.p025[i] : 0.0)
                      .arg(i < (int)s.p50.size()  ? s.p50[i]  : 0.0)
                      .arg(i < (int)s.p975.size() ? s.p975[i] : 0.0);
        QFile f(outputPath + "/posterior_ci_cycle_" + tag + ".txt");
        if (f.open(QIODevice::WriteOnly))
        {
            f.write(ci.toUtf8());
            f.close();
        }
        else
        {
            errorMessage = "cannot write posterior CI table";
            return false;
        }
    }

    {
        DTDebugLog &dlog = DTDebugLog::instance();
        if (dlog.enabled(DTDebugLog::Category::MCMC))
            dlog.log(DTDebugLog::Category::MCMC,
                     QString("posterior dist+CI: %1 params, %2 samples, "
                             "%3 bins at t_now=%4")
                         .arg(np).arg(N).arg(nbins).arg(tNow));
    }
    return true;
}

// ---------------------------------------------------------------------------
// writeProposalCovariance
//
// Archives the accumulated proposal covariance (proposal space: log
// coordinates for log-normal priors) as proposal_cov_cycle_<t>.txt.
// Analysis-only -- the sampler reads its covariance from the posterior
// snapshot, never from here. Without this the running estimate is visible
// only in the latest snapshot, which is overwritten every cycle, so its
// convergence over the run is unrecoverable.
//
// Emits the covariance, the derived correlation matrix and the marginal
// standard deviations, so a correlation heatmap needs no post-processing.
// ---------------------------------------------------------------------------
bool DTStreamingMCMC::writeProposalCovariance(const QString &outputPath,
                                              double tNow,
                                              QString &errorMessage)
{
    const int d = static_cast<int>(m_propCov.size());
    if (d == 0) return true;          // nothing accumulated yet: not an error

    const QString path = outputPath + "/proposal_cov_cycle_" +
                         QString::number(tNow, 'f', 0) + ".txt";
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        errorMessage = "cannot open proposal covariance file: " + path;
        return false;
    }

    std::vector<double> sd(d);
    for (int i = 0; i < d; ++i) sd[i] = std::sqrt(std::max(m_propCov[i][i], 0.0));

    QString out;
    out += QString("# proposal covariance, proposal space "
                   "(log coords for log-normal priors)\n");
    out += QString("# t_now=%1 cycle=%2 weight=%3 ready=%4 kappa=%5\n")
               .arg(tNow, 0, 'f', 5).arg(m_cycleIndex)
               .arg(m_propCovWeight).arg(m_propReady ? 1 : 0).arg(m_kappa);

    QStringList names;
    for (int i = 0; i < d; ++i)
        names << QString::fromStdString(parameter(i)->GetName());

    out += "sd," + names.join(',') + "\n";
    { QStringList v; for (int i = 0; i < d; ++i) v << QString::number(sd[i], 'g', 8);
      out += "sd," + v.join(',') + "\n"; }

    out += "\ncov," + names.join(',') + "\n";
    for (int i = 0; i < d; ++i)
    {
        QStringList v;
        for (int j = 0; j < d; ++j) v << QString::number(m_propCov[i][j], 'g', 8);
        out += names[i] + "," + v.join(',') + "\n";
    }

    out += "\ncorr," + names.join(',') + "\n";
    for (int i = 0; i < d; ++i)
    {
        QStringList v;
        for (int j = 0; j < d; ++j)
        {
            const double den = sd[i] * sd[j];
            v << QString::number(den > 0.0 ? m_propCov[i][j] / den : 0.0, 'g', 8);
        }
        out += names[i] + "," + v.join(',') + "\n";
    }

    file.write(out.toUtf8());
    file.close();
    return true;
}

// ---------------------------------------------------------------------------
// writeChainTraces
//
// Dumps every chain's full within-cycle log-posterior trajectory as
// chain_traces_cycle_<t>.csv. DTChainState::trace is a rolling deque capped
// at plateauWindow (60), so the climb->plateau trajectory that the
// classifier acts on is otherwise destroyed as the cycle proceeds; this is
// the only record of it. Columns:
//
//   chain    chain index
//   sweep    0-based step index within the cycle
//   logp     the chain's log-posterior AFTER the step (rejected steps
//            re-record the current level, matching classifier input)
//   accepted 1 if the step was accepted
//   phase    'C' climbing / 'P' plateaued, in force when the step was taken;
//            a 'P' row is one whose sample entered the pool
// ---------------------------------------------------------------------------
bool DTStreamingMCMC::writeChainTraces(const QString &outputPath,
                                       double tNow,
                                       QString &errorMessage)
{
    if (m_chains.empty()) return true;

    const QString path = outputPath + "/chain_traces_cycle_" +
                         QString::number(tNow, 'f', 0) + ".csv";
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        errorMessage = "cannot open chain trace file: " + path;
        return false;
    }

    QString out = "chain,sweep,logp,accepted,phase\n";
    for (size_t c = 0; c < m_chains.size(); ++c)
    {
        const DTChainState &ch = m_chains[c];
        for (size_t k = 0; k < ch.fullTraceLogp.size(); ++k)
        {
            out += QString("%1,%2,%3,%4,%5\n")
                       .arg(c).arg(k)
                       .arg(ch.fullTraceLogp[k], 0, 'g', 10)
                       .arg(k < ch.fullTraceAccepted.size()
                                ? int(ch.fullTraceAccepted[k]) : 0)
                       .arg(QChar(k < ch.fullTracePhase.size()
                                      ? ch.fullTracePhase[k] : 'C'));
        }
        if (out.size() > (1 << 20)) { file.write(out.toUtf8()); out.clear(); }
    }
    file.write(out.toUtf8());
    file.close();
    return true;
}

// ---------------------------------------------------------------------------
bool DTStreamingMCMC::appendParameterCIRow(const DTCycleResult &result,
                                           const QString &csvPath,
                                           double tNow,
                                           QString &errorMessage)
{
    const unsigned int np = MCMC_Settings.number_of_parameters;
    // Emit the five statistics whenever a summary was computed this cycle
    // (pool non-empty), regardless of certification. The `converged`
    // column still distinguishes a certified interval from a provisional
    // one; the interval numbers are published either way.
    const bool full = !result.summary.mean.empty();

    // Header (written once, on cycle 1 of a fresh deployment). m_cycleIndex
    // is already incremented to 1 by runCycle before this append runs, so the
    // first row truncates + emits the header; later rows append. Using <= 0
    // here would never write the header.
    const bool truncate = (m_cycleIndex <= 1);
    QString out;
    if (truncate)
    {
        out += "cycle,t_now,converged";
        for (unsigned int i = 0; i < np; ++i)
        {
            const QString p = QString::fromStdString(parameter(i)->GetName());
            out += QString(",%1_mean,%1_stdev,%1_p025,%1_p50,%1_p975").arg(p);
        }
        out += "\n";
    }

    out += QString("%1,%2,%3")
               .arg(m_cycleIndex)
               .arg(tNow, 0, 'f', 6)
               .arg(full ? 1 : 0);

    for (unsigned int i = 0; i < np; ++i)
    {
        if (full)
        {
            const DTPosteriorSummary &s = result.summary;
            out += QString(",%1,%2,%3,%4,%5")
                       .arg(s.mean[i], 0, 'g', 8)
                       .arg(s.stdev[i], 0, 'g', 8)
                       .arg(s.p025[i], 0, 'g', 8)
                       .arg(s.p50[i], 0, 'g', 8)
                       .arg(s.p975[i], 0, 'g', 8);
        }
        else
        {
            // Provisional: point estimate into mean/p50, CI columns blank.
            const double pe = (i < result.pointEstimate.size())
                                  ? result.pointEstimate[i] : 0.0;
            out += QString(",%1,,,%2,")
                       .arg(pe, 0, 'g', 8)
                       .arg(pe, 0, 'g', 8);
        }
    }
    out += "\n";

    QFile f(csvPath);
    const QIODevice::OpenMode mode =
        QIODevice::WriteOnly | (truncate ? QIODevice::Truncate
                                         : QIODevice::Append);
    if (!f.open(mode))
    {
        errorMessage = "cannot open parameter CI CSV: " + csvPath;
        return false;
    }
    f.write(out.toUtf8());
    f.close();
    return true;
}

// ---------------------------------------------------------------------------
bool DTStreamingMCMC::appendPooledSamples(const DTCycleResult &result,
                                          const QString &csvPath,
                                          double tNow,
                                          QString &errorMessage)
{
    const unsigned int np = MCMC_Settings.number_of_parameters;
    const std::vector<std::vector<double>> &pool = result.pooledSamples;
    if (pool.empty())
        return true;   // no post-plateau samples retained this cycle

    // Header once: fresh deployment's first cycle, or when the file does not
    // yet exist (feature enabled mid-run). Otherwise append. On resume
    // m_cycleIndex is restored > 1, so we append rather than clobber.
    const bool truncate   = (m_cycleIndex <= 1);
    const bool needHeader = truncate || !QFileInfo::exists(csvPath);

    QString out;
    if (needHeader)
    {
        out += "cycle,t_now,converged";
        for (unsigned int i = 0; i < np; ++i)
            out += "," + QString::fromStdString(parameter(i)->GetName());
        out += "\n";
    }

    // One row per pooled sample, tagged with its origin cycle.
    const QString prefix = QString("%1,%2,%3")
                               .arg(m_cycleIndex)
                               .arg(tNow, 0, 'f', 6)
                               .arg(result.converged ? 1 : 0);
    for (const std::vector<double> &samp : pool)
    {
        out += prefix;
        for (unsigned int i = 0; i < np; ++i)
            out += "," + QString::number(i < samp.size() ? samp[i] : 0.0, 'g', 8);
        out += "\n";
    }

    QFile f(csvPath);
    const QIODevice::OpenMode mode =
        QIODevice::WriteOnly | (truncate ? QIODevice::Truncate
                                         : QIODevice::Append);
    if (!f.open(mode))
    {
        errorMessage = "cannot open posterior samples CSV: " + csvPath;
        return false;
    }
    f.write(out.toUtf8());
    f.close();
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

    // Percentile fields are emitted for any cycle that produced a pooled
    // summary -- provisional cycles included. The `converged` flag above
    // still distinguishes a certified posterior width from a provisional
    // (uncertified) one, so downstream consumers can label the band
    // accordingly, but the dispersion numbers are published either way.
    if (!result.summary.mean.empty())
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

    // ------------------------------------------------------------------
    // Proposal-kernel state. None of this was recoverable post-hoc before:
    // kappa lived only in the (overwritten) snapshot and the debug log, and
    // the covariance-readiness flag nowhere at all. One line per cycle here
    // makes the kernel's whole trajectory plottable from the history file.
    // ------------------------------------------------------------------
    rec["kappa"]           = m_kappa;
    rec["prop_cov_ready"]  = m_propReady;
    rec["prop_cov_weight"] = m_propCovWeight;
    rec["pertcoeff"]       = toJsonArray(pertcoeff);
    rec["seed_mode"]       = seedModeName(m_lastSeedMode);

    // Pool quality. pool_size counts retained draws including the repeats a
    // rejected step re-pushes; the distinct count is the honest measure of
    // how much shape information the cycle actually contributed.
    {
        std::set<std::vector<double>> distinct(result.pooledSamples.begin(),
                                               result.pooledSamples.end());
        rec["unique_pool_size"] = static_cast<double>(distinct.size());
    }

    // Ensemble log-posterior spread at cycle end -- the natural convergence
    // diagnostic across chains, and otherwise only visible as debug-log text.
    if (!result.chainLogp.empty())
    {
        std::vector<double> lp = result.chainLogp;
        std::sort(lp.begin(), lp.end());
        rec["chain_logp_min"]    = lp.front();
        rec["chain_logp_median"] = lp[lp.size() / 2];
        rec["chain_logp_max"]    = lp.back();
    }

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
const char *DTStreamingMCMC::seedModeName(SeedMode m)
{
    switch (m)
    {
        case SeedMode::ColdStart:         return "cold_start";
        case SeedMode::WarmStart:         return "warm_start";
        case SeedMode::RatioReseed:       return "ratio_reseed";
        case SeedMode::ProvisionalResume: return "provisional_resume";
    }
    return "unknown";
}

bool DTStreamingMCMC::initializeCycle(SeedMode mode, QString &errorMessage)
{
    m_lastSeedMode = mode;

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

    // Reset the predictive-band output reservoir for this cycle.
    m_outputReservoir.reset(streamSettings.realizationCount);

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

    // ------------------------------------------------------------------
    // Adaptive-covariance proposal state.
    //
    // kappa MUST be positive before the first adaptProposalScale() call.
    // It was previously initialized lazily inside refactorProposalCholesky(),
    // which only runs at cycle END via accumulateProposalCovariance() -- so
    // during sampling kappa was still 0.0, every adaptation block computed
    // 0.0 * purt_change_scale = 0, and the clamp pinned it at the 1e-4 floor
    // for the whole run (~8000x below kappa_0).
    //
    // Restoring m_propCov here (and refactoring now, not at cycle end) is
    // what lets the covariance actually precondition proposals: m_propReady
    // must be true from the first sweep, not armed moments before the object
    // is destroyed.
    // ------------------------------------------------------------------
    m_kappa = (m_prevKappa > 0.0)
                  ? m_prevKappa
                  : 2.38 / std::sqrt(static_cast<double>(np));

    if (streamSettings.adaptiveCovariance && m_prevPropCov.size() == np
        && m_prevPropCovWeight > 0.0)
    {
        m_propCov       = m_prevPropCov;
        m_propCovWeight = m_prevPropCovWeight;
        refactorProposalCholesky();   // sets m_propChol and m_propReady now
    }
    else
    {
        m_propCov.clear();
        m_propChol.clear();
        m_propCovWeight = 0.0;
        m_propReady     = false;
    }

    {
        DTDebugLog &dlog = DTDebugLog::instance();
        if (dlog.enabled(DTDebugLog::Category::MCMC) &&
            streamSettings.adaptiveCovariance)
            dlog.log(DTDebugLog::Category::MCMC,
                     QString("proposal covariance: %1 (weight=%2, need>=%3), "
                             "kappa=%4 (%5)")
                         .arg(m_propReady ? "READY -- preconditioning proposals"
                                          : "not ready -- legacy walk")
                         .arg(m_propCovWeight)
                         .arg(2 * np)
                         .arg(m_kappa)
                         .arg(m_prevKappa > 0.0 ? "carried" : "init 2.38/sqrt(d)"));
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
        // Stop at whichever comes first: the sweep cap or the wall-clock
        // deadline. With a bounded (rolling) window the deadline rarely
        // binds, so maxSweeps becomes the effective control.
        if (streamSettings.maxSweeps > 0 &&
            m_sweepIndex >= streamSettings.maxSweeps)
            break;
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

    // Chain states must be populated BEFORE posteriorMAP: on a provisional
    // cycle the point estimate is the best carried chain state, which reads
    // result.chainParams / result.chainLogp. (Fix B moved the pointEstimate
    // assignment up so the stability ring sees the current cycle; the
    // chain-state fill has to move up with it, or posteriorMAP reads empty
    // vectors on every provisional cycle and returns no estimate.)
    result.chainParams.reserve(nc);
    result.chainLogp.reserve(nc);
    for (const DTChainState &chain : m_chains)
    {
        result.chainParams.push_back(chain.params);
        result.chainLogp.push_back(chain.logp);
    }

    // --- convergence certification (quorum OR inter-cycle stability) ---
    // Two independent paths to a FULL cycle:
    //   quorum    -- within-cycle: >= quorumFraction of chains plateaued this
    //                cycle. Compatible with genuine parameter drift.
    //   stability -- inter-cycle fallback: the point estimate has moved less
    //                than stabilityTol across the last stabilityWindow cycles
    //                AND >= stabilityMinPlateaued of chains plateaued. NOTE:
    //                this path presumes an approximately STATIONARY target,
    //                so it is at odds with drift detection -- it can be turned
    //                off with stabilityEnabled=false (mcmc_stability_enabled),
    //                or its strictness tuned via stabilityTol / stabilityWindow.
    // Push this cycle's point estimate onto the stability ring first, so
    // streamStable() sees the current cycle.
    result.pointEstimate = posteriorMAP(result);
    if (!result.pointEstimate.empty())
    {
        m_pointEstimateHistory.push_back(result.pointEstimate);
        while (static_cast<int>(m_pointEstimateHistory.size()) >
               streamSettings.stabilityWindow)
            m_pointEstimateHistory.pop_front();
    }

    const bool byQuorum = quorumHolds();
    const bool byStability =
        streamSettings.stabilityEnabled &&
        !byQuorum &&
        streamStable() &&
        plateauedFraction() >= streamSettings.stabilityMinPlateaued;

    result.converged         = byQuorum || byStability;
    result.convergenceSource = byQuorum ? "quorum"
                                        : (byStability ? "stability" : "");
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

    // Assemble the pool and compute summaries whenever any post-plateau
    // samples were retained this cycle -- including provisional cycles --
    // so that parameter CIs, distributions, and the predictive band are
    // published every cycle regardless of certification. On a provisional
    // cycle the pool is the retained samples of whichever chains happened
    // to plateau; its dispersion is an estimate from a partial ensemble,
    // NOT a certified posterior width. The distinction is preserved by
    // result.converged / convergenceSource in the record, so downstream
    // consumers can still tell a certified interval from a provisional
    // one, but the numbers are emitted either way.
    assemblePool(result);
    if (!result.pooledSamples.empty())
    {
        result.summary             = computeSummary(result.pooledSamples);
        result.effectiveSampleSize = effectiveSampleSize();
        // Fold this cycle's pool into the running proposal covariance so the
        // NEXT cycle proposes with the accumulated correlation structure.
        if (streamSettings.adaptiveCovariance)
            accumulateProposalCovariance(result);
    }
    // (result.pointEstimate was set above, before the stability ring push.)

    ++m_cycleIndex;
    logCycleDiagnostics(result);

    if (dlog.enabled(DTDebugLog::Category::MCMC))
        dlog.log(DTDebugLog::Category::MCMC,
                 QString("cycle %1 published: %2%8 plateaued=%3 pool=%4 "
                         "accept=%5 sweeps=%6 evals=%7")
                     .arg(m_cycleIndex)
                     .arg(result.converged ? "FULL" : "PROVISIONAL")
                     .arg(result.plateauedFraction)
                     .arg(result.pooledSamples.size())
                     .arg(result.acceptanceRate)
                     .arg(result.totalSweeps)
                     .arg(result.totalEvaluations)
                     .arg(result.convergenceSource.empty()
                              ? QString()
                              : QString(" via %1").arg(
                                    QString::fromStdString(result.convergenceSource))));

    DTDebugLog::instance().log(DTDebugLog::Category::MCMC,
                               QString("runCycle END: reservoir=%1 seen=%2 pool=%3 converged=%4")
                                   .arg(m_outputReservoir.outputs.size())
                                   .arg(m_outputReservoir.seen)
                                   .arg(result.pooledSamples.size())
                                   .arg(result.converged));
    DTDebugLog::instance().flush();

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
double DTStreamingMCMC::posteriorLocal(int c, const std::vector<double> &par,
                                       std::vector<TimeSeries<double>> *captureOut)
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

    // Capture modeled observation series before `work` is destroyed
    // (retained plateaued samples destined for the reservoir only).
    if (captureOut != nullptr)
    {
        const int nobs = static_cast<int>(work.ObservationsCount());
        captureOut->clear();
        captureOut->reserve(nobs);
        for (int i = 0; i < nobs; ++i)
            captureOut->push_back(*(work.observation(i)->GetModeledTimeSeries()));
    }

    return sum - work.GetObjectiveFunctionValue();
}

// ---------------------------------------------------------------------------
void DTStreamingMCMC::offerToReservoir(std::vector<TimeSeries<double>> &&modeled)
{
    std::lock_guard<std::mutex> lock(m_reservoirMutex);
    DTOutputReservoir &R = m_outputReservoir;
    ++R.seen;
    if (static_cast<int>(R.outputs.size()) < R.capacity)
    {
        R.outputs.push_back(std::move(modeled));   // still filling
        return;
    }
    // Reservoir full: replace a random slot with probability capacity/seen.
    std::uniform_int_distribution<long long> pick(0, R.seen - 1);
    const long long j = pick(m_seedRng);
    if (j < R.capacity)
        R.outputs[static_cast<size_t>(j)] = std::move(modeled);
    // else: discard the newcomer
}

// ---------------------------------------------------------------------------
bool DTStreamingMCMC::stepChain(int c, std::mt19937_64 &rng)
{
    DTChainState &chain = m_chains[c];

    double logJ = 0.0;
    std::vector<double> X = proposeFrom(chain.params, rng, logJ);

    // Capture modeled outputs on this solve only when the chain is already
    // plateaued (its accepted samples will be retained and are reservoir
    // candidates). Climbing chains skip capture -- their samples never pool.
    const bool capture = (chain.phase == ChainPhase::Plateaued);
    std::vector<TimeSeries<double>> modeled;

    // Pure log-posterior: log-prior + kernel-weighted log-likelihood.
    // One forward solve; the dominant cost of the step.
    const double logp0 = posteriorLocal(c, X, capture ? &modeled : nullptr);

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

    // Uncapped mirror of the above, for post-hoc analysis only. The phase
    // recorded here is the one in force when the step was taken, i.e.
    // before classifyChain() runs for this sweep -- so a 'P' row is a step
    // whose sample was actually retained.
    chain.fullTraceLogp.push_back(chain.logp);
    chain.fullTraceAccepted.push_back(accepted ? 1 : 0);
    chain.fullTracePhase.push_back(
        chain.phase == ChainPhase::Plateaued ? 'P' : 'C');

    // Burn-in exclusion at retention time (Sec. 3.4): only a plateaued
    // chain's states enter the retained store. Rejected steps repeat the
    // current state -- required for correct MH sample density.
    if (chain.phase == ChainPhase::Plateaued)
    {
        chain.samples.push_back(chain.params);
        chain.sampleLogp.push_back(chain.logp);

        // Reservoir candidate. On acceptance `modeled` holds the proposal's
        // outputs; on rejection it holds the (still current) state's outputs
        // only if this step captured -- but a rejected step's `modeled` is
        // the REJECTED proposal, not the retained state, so only offer when
        // the step was accepted (retained state == proposal == modeled).
        const bool offered = accepted && capture && !modeled.empty();
        if (offered)
            offerToReservoir(std::move(modeled));

        if (DTDebugLog::instance().enabled(DTDebugLog::Category::MCMCTrace))
        {
            if (offered)
                DTDebugLog::instance().log(DTDebugLog::Category::MCMCTrace,
                                           QString("reservoir offer: chain=%1 sweep=%2").arg(c).arg(m_sweepIndex));
            else if (chain.phase == ChainPhase::Plateaued)
                DTDebugLog::instance().log(DTDebugLog::Category::MCMCTrace,
                                           QString("reservoir SKIP: chain=%1 accepted=%2 capture=%3 modeled_empty=%4")
                                               .arg(c).arg(accepted).arg(capture).arg(modeled.empty()));
        }
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

    // --- adaptive-covariance proposal: X = x + kappa * L z, in the per-
    // coordinate proposal space (log for log-normal). Falls through to the
    // legacy per-coordinate walk until the running covariance is ready. ---
    if (streamSettings.adaptiveCovariance && m_propReady)
    {
        const int d = static_cast<int>(np);
        std::vector<double> z(d);
        for (int i = 0; i < d; ++i) z[i] = N(rng);
        for (int i = 0; i < d; ++i)
        {
            double delta = 0.0;                 // (L z)_i, L lower-triangular
            for (int k = 0; k <= i; ++k) delta += m_propChol[i][k] * z[k];
            delta *= m_kappa;
            if (parameter(i)->GetPriorDistribution() == "log-normal")
            {
                X[i] = x[i] * std::exp(delta);  // log-space move
                logJacobian += delta;           // = log(X_i/x_i)
            }
            else
            {
                X[i] = x[i] + delta;            // symmetric (no correction)
            }
        }
        return X;
    }

    // --- legacy per-coordinate global-scale random walk ---
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
// Accumulate this cycle's mean-centered posterior covariance (in proposal
// space) into a running, all-cycles estimate. The correlation structure is
// model-determined and stationary, so no forgetting is applied; per-cycle
// mean-centering removes the drifting location. Weighted by (n-1).
// ---------------------------------------------------------------------------
void DTStreamingMCMC::accumulateProposalCovariance(const DTCycleResult &result)
{
    const int d = static_cast<int>(MCMC_Settings.number_of_parameters);
    const std::vector<std::vector<double>> &S = result.pooledSamples;
    const int n = static_cast<int>(S.size());
    if (d <= 0 || n < d + 2) return;   // too few draws to estimate a d-dim covariance

    std::vector<char> isLog(d);
    for (int i = 0; i < d; ++i)
        isLog[i] = (parameter(i)->GetPriorDistribution() == "log-normal") ? 1 : 0;

    // Transform each sample to proposal space (log for log-normal coords).
    std::vector<std::vector<double>> Z(n, std::vector<double>(d, 0.0));
    for (int s = 0; s < n; ++s)
        for (int i = 0; i < d; ++i)
        {
            const double v = (i < static_cast<int>(S[s].size())) ? S[s][i] : 0.0;
            Z[s][i] = isLog[i] ? std::log(std::max(v, 1e-300)) : v;
        }

    std::vector<double> mean(d, 0.0);
    for (const auto &z : Z) for (int i = 0; i < d; ++i) mean[i] += z[i];
    for (int i = 0; i < d; ++i) mean[i] /= n;

    // Cycle covariance Sigma_k (n-1 denominator), symmetric.
    std::vector<std::vector<double>> Sk(d, std::vector<double>(d, 0.0));
    for (const auto &z : Z)
        for (int i = 0; i < d; ++i)
        {
            const double di = z[i] - mean[i];
            for (int j = i; j < d; ++j) Sk[i][j] += di * (z[j] - mean[j]);
        }
    const double w = n - 1;
    for (int i = 0; i < d; ++i)
        for (int j = i; j < d; ++j) { Sk[i][j] /= w; Sk[j][i] = Sk[i][j]; }

    // Running weighted mean of per-cycle covariances (no forgetting).
    if (static_cast<int>(m_propCov.size()) != d)
    {
        m_propCov.assign(d, std::vector<double>(d, 0.0));
        m_propCovWeight = 0.0;
    }
    const double W = m_propCovWeight;
    for (int i = 0; i < d; ++i)
        for (int j = 0; j < d; ++j)
            m_propCov[i][j] = (W * m_propCov[i][j] + w * Sk[i][j]) / (W + w);
    m_propCovWeight = W + w;

    refactorProposalCholesky();
}

// Cholesky of (m_propCov + ridge); sets m_propChol and m_propReady.
void DTStreamingMCMC::refactorProposalCholesky()
{
    const int d = static_cast<int>(m_propCov.size());
    if (d == 0) { m_propReady = false; return; }

    double tr = 0.0;
    for (int i = 0; i < d; ++i) tr += m_propCov[i][i];
    const double ridge = std::max(1e-12, 1e-8 * tr / d);   // positive-definiteness

    std::vector<std::vector<double>> A = m_propCov;
    for (int i = 0; i < d; ++i) A[i][i] += ridge;

    std::vector<std::vector<double>> L(d, std::vector<double>(d, 0.0));
    bool ok = true;
    for (int i = 0; i < d && ok; ++i)
        for (int j = 0; j <= i; ++j)
        {
            double sum = A[i][j];
            for (int k = 0; k < j; ++k) sum -= L[i][k] * L[j][k];
            if (i == j)
            {
                if (sum <= 0.0) { ok = false; break; }
                L[i][j] = std::sqrt(sum);
            }
            else
            {
                L[i][j] = sum / L[j][j];
            }
        }

    if (ok)
    {
        m_propChol = std::move(L);
        if (m_kappa <= 0.0) m_kappa = 2.38 / std::sqrt(static_cast<double>(d));
        m_propReady = (m_propCovWeight >= 2 * d);   // enough accumulated
    }
    else
    {
        m_propReady = false;   // fall back to the global proposal this cycle
    }
}

// ---------------------------------------------------------------------------
// Plateau classifier and quorum (Sec. 3.2)
// ---------------------------------------------------------------------------
void DTStreamingMCMC::classifyChain(int c, qint64 sweepIndex)
{
    DTChainState &chain = m_chains[c];

    // Effective window: never larger than the configured plateauWindow,
    // but capped at half the sweeps elapsed so far so that late, short
    // cycles (whose sweep budget is smaller than plateauWindow as the
    // record grows, Sec. 2.3) can still fill a window and classify.
    // Without this, a 42-sweep cycle can never accumulate a 60-step
    // trace and every chain is stuck "climbing" purely for lack of data.
    const int effWindow = std::min(
        streamSettings.plateauWindow,
        std::max(10, static_cast<int>(sweepIndex) / 2));

    // Trend is tested over the last effWindow trace points.
    const int have = static_cast<int>(chain.trace.size());
    const int n    = std::min(have, effWindow);

    // Minimum data to classify: the smaller of the configured floor and
    // the effective window (so a small effWindow doesn't deadlock on a
    // larger minStepsBeforeClassify).
    const int minSteps = std::min(streamSettings.minStepsBeforeClassify, effWindow);
    if (n < minSteps)
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

    // Least-squares slope of logp against step index over the last n
    // trace points. With k = 0..n-1: b = sum((k-kbar)(y-ybar)) / sum((k-kbar)^2).
    const int start = have - n;   // tail offset into the trace deque
    const double kbar = 0.5 * (n - 1);
    double ybar = 0.0;
    for (int k = 0; k < n; ++k) ybar += chain.trace[start + k];
    ybar /= n;

    double sxy = 0.0, sxx = 0.0;
    for (int k = 0; k < n; ++k)
    {
        const double dk = k - kbar;
        sxy += dk * (chain.trace[start + k] - ybar);
        sxx += dk * dk;
    }
    const double b = sxy / sxx;

    // Scatter of the residuals about the fitted line (n-2 dof).
    double ss = 0.0;
    for (int k = 0; k < n; ++k)
    {
        const double r = (chain.trace[start + k] - ybar) - b * (k - kbar);
        ss += r * r;
    }
    const double s = std::sqrt(ss / std::max(n - 2, 1));

    // Stagnation guard: a window carried by too few ACCEPTED moves is a
    // stuck chain, not a plateau -- its trace is flat because nothing
    // moved, and treating that as convergence is what produced the
    // spurious "PLATEAUED at sweep 8, scatter=1e-13" events. Requires a
    // minimum fraction of accepts in the window before a plateau can be
    // declared. (Trajectory-plus-motion, still never level-based.)
    int acceptedInTrace = 0;
    {
        const int aStart = static_cast<int>(chain.acceptTrace.size()) - n;
        for (int k = 0; k < n; ++k)
            if (aStart + k >= 0) acceptedInTrace += chain.acceptTrace[aStart + k];
    }
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

bool DTStreamingMCMC::streamStable() const
{
    const int W = streamSettings.stabilityWindow;
    if (static_cast<int>(m_pointEstimateHistory.size()) < W) return false;

    const size_t np = m_pointEstimateHistory.front().size();
    for (const auto &pe : m_pointEstimateHistory)
        if (pe.size() != np) return false;   // schema changed mid-window

    // Per parameter: spread across the ring relative to its recent mean.
    for (size_t i = 0; i < np; ++i)
    {
        double mean = 0.0, lo = 1e300, hi = -1e300;
        for (const auto &pe : m_pointEstimateHistory)
        {
            const double v = pe[i];
            mean += v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        mean /= static_cast<double>(m_pointEstimateHistory.size());

        // Scale by |recent mean|, with a small floor so parameters that
        // sit near zero (e.g. a conductivity driven to its lower bound)
        // don't trip the test on absolute noise.
        const double scale = std::max(std::fabs(mean), 1e-9);
        if ((hi - lo) / scale > streamSettings.stabilityTol)
            return false;
    }
    return true;
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
                             "(pertcoeff[0]=%4, kappa=%5, cov=%6)")
                         .arg(blockRate)
                         .arg(MCMC_Settings.acceptance_rate)
                         .arg(quorumHolds()
                                  ? "FROZEN (quorum holds)"
                                  : (blockRate > MCMC_Settings.acceptance_rate
                                         ? "-> enlarge steps"
                                         : "-> shrink steps"))
                         .arg(pertcoeff.empty() ? 0.0 : pertcoeff[0])
                         .arg(m_kappa)
                         .arg(streamSettings.adaptiveCovariance
                                  ? (m_propReady ? "ready" : "not-ready")
                                  : "off"));
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

    // Adaptive-covariance mode: tune the SINGLE global scalar kappa; the
    // proposal SHAPE comes from the accumulated covariance, so kappa can no
    // longer freeze the broad directions chasing the sharp one.
    if (streamSettings.adaptiveCovariance)
    {
        if (rate > MCMC_Settings.acceptance_rate)
            m_kappa /= MCMC_Settings.purt_change_scale;   // enlarge steps
        else
            m_kappa *= MCMC_Settings.purt_change_scale;   // shrink steps
        m_kappa = std::min(std::max(m_kappa, 1e-4), 1e3); // floor/ceiling
        return;
    }

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

namespace
{
// ESS of a single scalar chain via Geyer's initial monotone sequence.
// Returns n / (1 + 2*sum rho_k), with the autocovariance sum truncated
// where consecutive lag-pair sums stop being positive (the standard
// robust truncation for MCMC). Returns n for n < 8 (too short to
// estimate autocorrelation; treat as independent, conservatively small).
double chainESS(const std::vector<double> &x)
{
    const int n = static_cast<int>(x.size());
    if (n < 8) return static_cast<double>(std::max(0, n));

    double mean = 0.0;
    for (double v : x) mean += v;
    mean /= n;

    // Autocovariance gamma_k for k = 0..maxLag.
    const int maxLag = n - 1;
    std::vector<double> gamma(maxLag + 1, 0.0);
    for (int k = 0; k <= maxLag; ++k)
    {
        double s = 0.0;
        for (int i = 0; i + k < n; ++i)
            s += (x[i] - mean) * (x[i + k] - mean);
        gamma[k] = s / n;
    }
    if (gamma[0] <= 0.0) return static_cast<double>(n);   // constant chain

    // Geyer initial positive/monotone sequence: pair adjacent lags
    // (Gamma_m = gamma_{2m} + gamma_{2m+1}); stop at the first
    // non-positive pair, and enforce monotone non-increasing pairs.
    double sumPairs = 0.0;
    double prevPair = std::numeric_limits<double>::max();
    for (int m = 0; 2 * m + 1 <= maxLag; ++m)
    {
        double pair = gamma[2 * m] + gamma[2 * m + 1];
        if (pair <= 0.0) break;
        if (pair > prevPair) pair = prevPair;   // monotone clamp
        sumPairs += pair;
        prevPair = pair;
    }

    // tau = 1 + 2*sum_{k>=1} rho_k = (2*sumPairs - gamma_0) / gamma_0.
    const double tau = (2.0 * sumPairs - gamma[0]) / gamma[0];
    if (tau < 1.0) return static_cast<double>(n);         // anti-correlated => cap at n
    return n / tau;
}
} // namespace

double DTStreamingMCMC::effectiveSampleSize() const
{
    // Per parameter: sum per-chain ESS (each chain's retained samples are
    // a contiguous, correctly-ordered MCMC subsequence). Report the min
    // across parameters -- the worst-mixing direction (typically a
    // correlated ridge) bounds how much independent information the pool
    // actually carries.
    const unsigned int np = MCMC_Settings.number_of_parameters;
    if (np == 0) return 0.0;

    double essMin = std::numeric_limits<double>::max();
    for (unsigned int i = 0; i < np; ++i)
    {
        double essParam = 0.0;
        for (const DTChainState &chain : m_chains)
        {
            if (chain.samples.empty()) continue;
            std::vector<double> col;
            col.reserve(chain.samples.size());
            for (const std::vector<double> &s : chain.samples)
                col.push_back(s[i]);
            essParam += chainESS(col);
        }
        essMin = std::min(essMin, essParam);
    }
    return (essMin == std::numeric_limits<double>::max()) ? 0.0 : essMin;
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
