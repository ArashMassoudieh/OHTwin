/*
 * OpenHydroTwin
 * Copyright (C) 2026  EnviroInformatics, LLC
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

#include "DTRunner.h"

#include "System.h"
#include "Script.h"
#include "DTAssimilation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "VizRenderer.h"
#include "DTWeather.h"
#include <QThread>
#include "RunLogger.h"

// ---------------------------------------------------------------------------
// OHQ epoch: day-serial 0 = 1899-12-30 (Excel convention)
// ---------------------------------------------------------------------------
static const QDate kOHQEpoch(1899, 12, 30);


// ---------------------------------------------------------------------------
// selected_output.csv ordering helpers
// Keep viewer panels in a user-facing order independent of the order in OHQ
// returns observations or the order preserved by an existing selected_output.csv.
// ---------------------------------------------------------------------------
namespace
{
static std::string dtLowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool dtContains(const std::string &haystackLower, const std::string &needleLower)
{
    return haystackLower.find(needleLower) != std::string::npos;
}

static int dtSelectedOutputRank(const std::string &seriesName)
{
    const std::string n = dtLowerCopy(seriesName);

    // Requested VN observation order:
    //   0) precipitation / rainfall forcing
    //   1) inflow into top well
    //   2) top well head/storage
    //   3) bottom well depth/storage
    //   4) recharge / flow
    //   5) groundwater
    //   6) ERT-like soil moisture points

    if (dtContains(n, "precip") || dtContains(n, "rain"))
        return -10;

    if (dtContains(n, "inflow") || dtContains(n, "catchment to well"))
        return 0;

    if (dtContains(n, "well_c") || dtContains(n, "well c"))
        return 10;

    if (dtContains(n, "well_g") || dtContains(n, "well g"))
        return 20;

    if (dtContains(n, "recharge"))
        return 30;

    if (dtContains(n, "flow"))
        return 40;

    if (dtContains(n, "groundwater") || dtContains(n, "ground water") || dtContains(n, "gw"))
        return 80;

    if (dtContains(n, "ert5"))
        return 90;

    if (dtContains(n, "ert3"))
        return 91;

    if (dtContains(n, "ert") || dtContains(n, "theta") || dtContains(n, "soil moisture"))
        return 92;

    return 50;
}

static bool dtNeedsCustomOrdering(const std::string &deploymentName)
{
    const std::string d = dtLowerCopy(deploymentName);

    // Keep historical custom viewer ordering only for these deployments.
    // All other models preserve the original OHQ observation/output order.
    return d == "vn"
           || d == "vn_drywell"
           || d == "r"
           || d == "hq"
           || d == "hq_drywell";
}

static TimeSeriesSet<double> dtReorderSelectedOutput(
    const TimeSeriesSet<double> &src,
    const std::string &deploymentName)
{
    if (!dtNeedsCustomOrdering(deploymentName))
        return src;

    std::vector<size_t> idx(src.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;

    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        const int ra = dtSelectedOutputRank(src[a].name());
        const int rb = dtSelectedOutputRank(src[b].name());

        if (ra != rb)
            return ra < rb;

        // Preserve original OHQ order inside each priority group.
        return a < b;
    });

    TimeSeriesSet<double> out;
    for (size_t i : idx)
        out.push_back(src[i]);

    return out;
}
}

// ---------------------------------------------------------------------------
// ctor
// ---------------------------------------------------------------------------
DTRunner::DTRunner(const DTConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
    // Pre-compute interval length in OHQ day units
    m_intervalDays  = static_cast<double>(config.intervalMs) / (86400.0 * 1000.0);
    m_forecastDays  = static_cast<double>(config.forecastHorizonMs) / (86400.0 * 1000.0);

    // Pre-compute observation save cadence in OHQ day units. If the
    // observations block was omitted, DTConfig::load() already defaulted
    // saveIntervalMs to intervalMs, so this falls through to the model
    // interval naturally. The defensive >0 guard is just belt-and-suspenders
    // against a future code path that might leave it zero.
    m_obsSaveIntervalDays =
        static_cast<double>(config.observations.saveIntervalMs) / (86400.0 * 1000.0);
    if (m_obsSaveIntervalDays <= 0.0)
        m_obsSaveIntervalDays = m_intervalDays;

    std::cout << "[Runner] obs save interval : "
              << m_obsSaveIntervalDays << " day(s)\n";
    const bool obsNoiseEnabled =
        (config.observations.noiseSigma > 0.0) ||
        !config.observations.noiseSigmaByPattern.empty();

    if (obsNoiseEnabled)
    {
        const double tauDays =
            static_cast<double>(config.observations.noiseCorrelationTimeMs)
            / (86400.0 * 1000.0);
        std::cout << "[Runner] obs noise default : "
                  << config.observations.noiseSigma
                  << (config.observations.noiseMode == NoiseMode::Additive
                          ? "  (ADDITIVE, sigma in observation units)"
                          : "  (multiplicative, sigma relative)") << "\n";
        if (!config.observations.noiseSigmaByPattern.empty())
        {
            std::cout << "[Runner] obs noise by name : ";
            for (const auto &kv : config.observations.noiseSigmaByPattern)
                std::cout << "'" << kv.first << "'=" << kv.second << " ";
            std::cout << "\n";
        }
        std::cout << "[Runner] obs noise tau     : " << tauDays
                  << " day(s)";
        if (tauDays <= 0.0) std::cout << " (white-noise limit)";
        std::cout << "\n";
    }
    else
    {
        std::cout << "[Runner] obs noise         : disabled (sigma=0)\n";
    }
}

// Out-of-line destructor: required so that unique_ptr<DTAssimilation>'s
// destruction can see the full DTAssimilation type (declared via the
// #include at the top of this file).
DTRunner::~DTRunner()
{
    if (m_assimThread)
    {
        m_assimThread->quit();
        m_assimThread->wait();
    }
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
bool DTRunner::init(QString &errorMessage)
{
    // Validate script file exists
    if (!m_config.loadModelJson.empty())
    {
        // hot-restart path checked at run-time (snapshot may not exist yet)
    }
    else
    {
        const QFileInfo fi(QString::fromStdString(m_config.scriptFile));
        if (!fi.exists())
        {
            errorMessage = "Script file not found: " +
                           QString::fromStdString(m_config.scriptFile);
            return false;
        }
    }

    // Determine the wall-clock start time for the first interval.
    //
    // RESUME TAKES PRECEDENCE over the configured start. If a prior state
    // snapshot survives with a next-start time, this is a restart without
    // --fresh (--fresh wipes state/ before we get here), so we continue
    // exactly where the run left off. start_datetime is the COLD-START anchor
    // only: it is used when there is no snapshot to resume from -- a genuine
    // first run, or a deliberate --fresh restart. This makes an interrupted
    // run resumable simply by relaunching without --fresh, even though
    // start_datetime is set in config.
    {
        const QString latestSnapshot = findLatestStateSnapshot();
        if (!latestSnapshot.isEmpty())
        {
            const QJsonObject state = readJson(latestSnapshot);
            const QString nextStart = state.value("_dt_next_start_utc").toString();
            if (!nextStart.isEmpty())
            {
                const QDateTime t = QDateTime::fromString(nextStart, Qt::ISODate);
                if (t.isValid())
                    m_nextIntervalStart = t;
            }
        }
    }

    if (m_nextIntervalStart.isValid())
    {
        std::cout << "[Runner] Resuming from previous state; next interval start: "
                  << m_nextIntervalStart.toString(Qt::ISODate).toStdString() << "\n";
    }
    else if (!m_config.startDatetime.empty())
    {
        // Cold start at the configured anchor (first run or after --fresh).
        m_nextIntervalStart = QDateTime::fromString(
            QString::fromStdString(m_config.startDatetime), Qt::ISODate);
        if (!m_nextIntervalStart.isValid())
        {
            errorMessage = "start_datetime in config is not a valid ISO 8601 string: " +
                           QString::fromStdString(m_config.startDatetime);
            return false;
        }
        m_nextIntervalStart.setTimeSpec(Qt::UTC);
        std::cout << "[Runner] Cold start at config start_datetime: "
                  << m_config.startDatetime << "\n";
    }
    else
    {
        // No snapshot and no configured anchor: begin from now.
        m_nextIntervalStart = QDateTime::currentDateTimeUtc();
        std::cout << "[Runner] No prior state or start_datetime; starting from now: "
                  << m_nextIntervalStart.toString(Qt::ISODate).toStdString() << "\n";
    }

    // ------------------------------------------------------------------
    // Data assimilation (optional). Constructed only if the assimilation
    // block is present in config.json. Failure here is fatal: we don't
    // want to silently run a forward-only twin when the operator
    // explicitly configured assimilation.
    //
    // The assimilation object is moved to its own QThread so that long
    // GA cycles don't block the forward loop's QTimer events on the
    // main thread.  Snapshot handoff in both directions uses signal/slot
    // connections, which become QueuedConnection automatically across
    // threads — preserving Qt's invariant that a slot only runs on the
    // thread its owning QObject lives on.
    // ------------------------------------------------------------------


    m_runLogger.reset(new RunLogger(QString::fromStdString(m_config.outputDir)));


    if (m_config.assimilation.enabled)
    {

        m_assimilation.reset(new DTAssimilation(m_config, nullptr));   // create first
        m_assimilation->setRunLogger(m_runLogger.get());
        // Construct on the main thread with parent=nullptr.  A QObject with
        // a parent cannot be moved to another thread, so we keep parentage
        // off and rely on unique_ptr for lifetime.


        // Validate config and perform the initial buffer refresh on the
        // main thread.  Must happen before moveToThread() so that any
        // configuration error fails fast at startup.
        QString assimErr;
        if (!m_assimilation->configure(assimErr))
        {
            errorMessage = "DTAssimilation::configure() failed: " + assimErr;
            return false;
        }

        // Diagnostic logging on poll outcomes.  Cross-thread signals
        // become QueuedConnection automatically, so these lambda bodies
        // run on the main thread — safe to use std::cout/std::cerr.
        QObject::connect(m_assimilation.get(), &DTAssimilation::buffered,
                         this, [](qint64 n) {
                             std::cout << "[Assim] poll OK — " << n << " points buffered\n";
                         });
        QObject::connect(m_assimilation.get(), &DTAssimilation::pollFailed,
                         this, [](const QString &err) {
                             std::cerr << "[Assim] poll failed: " << err.toStdString() << "\n";
                         });

        // Calibration result handoff: assim thread → main thread (queued).
        // The slot writes m_pendingCalibratedSnapshot, which is read at
        // the top of the next runOnce() on the main thread.
        QObject::connect(m_assimilation.get(),
                         &DTAssimilation::calibrationCompleted,
                         this,
                         &DTRunner::onCalibrationCompleted);

        // Forward → assim snapshot handoff: main thread → assim thread
        // (queued).  Replaces the former direct setLatestSnapshot() call
        // at the end of runOnce().
        QObject::connect(this, &DTRunner::snapshotReady,
                         m_assimilation.get(),
                         &DTAssimilation::setLatestSnapshot);

        // Move the assimilation object to its own thread.  The QTimer
        // owned by m_assimilation moves with it (it's a value member).
        // The QThread itself is parented to *this* so it's destroyed
        // when DTRunner is; ~DTRunner() calls quit()/wait() on it before
        // m_assimilation is destroyed via unique_ptr.
        m_assimThread = new QThread(this);
        m_assimilation->moveToThread(m_assimThread);

        // Once the thread's event loop is running, start the poll timer
        // on that thread.  Cannot start the timer earlier because QTimer
        // must be started from the thread it lives on.
        QObject::connect(m_assimThread, &QThread::started,
                         m_assimilation.get(),
                         &DTAssimilation::startTimer,
                         Qt::QueuedConnection);

        // Symmetric shutdown: stop the timer when the thread is asked
        // to quit (before the event loop exits).
        QObject::connect(m_assimThread, &QThread::finished,
                         m_assimilation.get(),
                         &DTAssimilation::stopTimer);

        m_assimThread->start();
    }
    else
    {
        std::cout << "[Runner] Assimilation disabled (no 'assimilation' block in config).\n";
    }

    // ------------------------------------------------------------------
    // Parameter drift (Truth Twin). Load each configured CSV into a
    // TimeSeries<double>. The Advance stage queries these by simulated
    // time and applies the interpolated value to the named parameter
    // before each Solve(). Empty config => no drift, vector stays empty.
    // ------------------------------------------------------------------
    m_parameterDrift.clear();
    m_parameterDrift.reserve(m_config.parameterDrift.size());
    for (const auto &entry : m_config.parameterDrift)
    {
        TimeSeries<double> ts;
        if (!ts.readfile(entry.csvPath))
        {
            errorMessage = "Failed to read parameter drift CSV: " +
                           QString::fromStdString(entry.csvPath);
            return false;
        }
        std::cout << "[Runner] Parameter drift loaded: '"
                  << entry.parameter << "' from "
                  << entry.csvPath << " ("
                  << ts.size() << " points)\n";
        m_parameterDrift.push_back(std::move(ts));
    }
    if (!m_parameterDrift.empty())
        std::cout << "[Runner] " << m_parameterDrift.size()
                  << " parameter drift series active.\n";


    return true;
}

// ---------------------------------------------------------------------------
// renderOnly
// Regenerates SVG files without running OHQ, fetching weather, updating state
// snapshots, or touching the selected-output stream.
//
// Preferred path:
//   outputs/viz_state.json          + deployment viz_*.json -> outputs/viz.svg
//   outputs/forecast_viz_state.json + deployment viz_*.json -> outputs/forecast_viz.svg
//
// Static fallback:
//   If a viz-state JSON file does not exist (or is empty/invalid), render from
//   an empty QJsonObject. This lets a deployment produce a static layout SVG
//   directly from config.json + viz_*.json before any simulation has run.
// ---------------------------------------------------------------------------
bool DTRunner::renderOnly()
{
    const QString vizJsonPath = QString::fromStdString(m_config.vizFile);
    const QString outputDir   = QString::fromStdString(m_config.outputDir);

    QDir().mkpath(outputDir);

    const QFileInfo vizInfo(vizJsonPath);
    if (!vizInfo.exists() || !vizInfo.isFile())
    {
        std::cerr << "[Runner] render-only: viz spec not found: "
                  << vizJsonPath.toStdString() << "\n";
        return false;
    }

    bool okAny = false;

    auto renderOne = [&](const QString &statePath,
                         const QString &svgPath,
                         const char *label) -> bool
    {
        QJsonObject fullState;
        const QFileInfo stateInfo(statePath);

        if (stateInfo.exists() && stateInfo.isFile())
        {
            fullState = readJson(statePath);
            if (fullState.isEmpty())
            {
                std::cout << "[Runner] render-only: " << label
                          << " state is empty/invalid; rendering static layout from "
                          << vizJsonPath.toStdString() << "\n";
            }
            else
            {
                std::cout << "[Runner] render-only: using "
                          << statePath.toStdString() << "\n";
            }
        }
        else
        {
            std::cout << "[Runner] render-only: " << label
                      << " state not found; rendering static layout from "
                      << vizJsonPath.toStdString() << "\n";
        }

        QString err;
        if (!VizRenderer::render(vizJsonPath, fullState, svgPath, err))
        {
            std::cerr << "[Runner] render-only " << label << " warning: "
                      << err.toStdString() << "\n";
            return false;
        }

        std::cout << "[Runner] render-only wrote: "
                  << svgPath.toStdString() << "\n";
        return true;
    };

    okAny = renderOne(outputDir + "/viz_state.json",
                      outputDir + "/viz.svg",
                      "advance") || okAny;

    okAny = renderOne(outputDir + "/forecast_viz_state.json",
                      outputDir + "/forecast_viz.svg",
                      "forecast") || okAny;

    return okAny;
}

// ---------------------------------------------------------------------------
// runOnce  (orchestrator: Advance, then optional Forecast)
// ---------------------------------------------------------------------------
bool DTRunner::runOnce()
{
    const QDateTime advanceStart = m_nextIntervalStart;
    QDateTime       advanceEnd;

    if (m_config.advanceToObservations &&
        m_assimilation &&
        m_assimilation->bufferPointCount() > 0)
    {
        const double tMaxSerial = m_assimilation->bufferTMax();
        const QDateTime tMaxDt  = fromOHQDaySerial(tMaxSerial);

        if (tMaxDt > advanceStart)
        {
            // Catch up to the data frontier in one variable-width step.
            advanceEnd = tMaxDt;
            const double daysCovered =
                advanceStart.msecsTo(tMaxDt) / 86400000.0;
            std::cout << "[Runner] catch-up mode: advancing to data frontier "
                      << "(" << daysCovered << " simulated days this cycle)\n";
        }
        else
        {
            // Assim is already at or past the observation frontier. Don't
            // advance simulated time — wait for truth to publish more data
            // before doing any forward work. No state snapshot, no output
            // file, no viz update this cycle. The next QTimer tick will
            // re-check the buffer.
            const double daysAhead =
                tMaxDt.msecsTo(advanceStart) / 86400000.0;
            std::cout << "[Runner] waiting for observations (assim at "
                      << advanceStart.toString(Qt::ISODate).toStdString()
                      << ", buffer t_max = "
                      << tMaxDt.toString(Qt::ISODate).toStdString()
                      << ", " << daysAhead << " days ahead). Skipping cycle.\n";
            return true;
        }
    }
    else
    {
        advanceEnd = advanceStart.addMSecs(m_config.intervalMs);
    }

    std::cout << "\n[Runner] ======== Cycle " << (m_runsCompleted + 1) << " ========\n";


    // Determine the snapshot to load for this cycle. A calibration that
    // completed since the previous cycle takes precedence over the
    // latest forward snapshot, since its parameters are more current.
    QString latestSnapshot;
    const QString cycleStamp =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    if (!m_pendingCalibratedSnapshot.isEmpty() &&
        QFileInfo::exists(m_pendingCalibratedSnapshot))
    {
        latestSnapshot = m_pendingCalibratedSnapshot;
        std::cout << "[Runner] [" << cycleStamp.toStdString() << "] "
                  << "CONSUMING calibrated snapshot: "
                  << m_pendingCalibratedSnapshot.toStdString() << "\n";

        // DIAG: read parameter values from the calibrated snapshot so we can
        // confirm the calibrated values are flowing into the forward cycle.
        QJsonObject snap = readJson(m_pendingCalibratedSnapshot);
        if (!snap.isEmpty() && snap.contains("Parameters"))
        {
            std::cout << "[Runner]   parameters in calibrated snapshot:\n";
            const QJsonObject params = snap["Parameters"].toObject();
            for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            {
                const QJsonObject p = it.value().toObject();
                std::cout << "[Runner]     " << it.key().toStdString()
                          << " = " << p["value"].toString().toStdString()
                          << " [" << p["low"].toString().toStdString()
                          << ", " << p["high"].toString().toStdString() << "]\n";
            }
        }
        m_pendingCalibratedSnapshot.clear();
    }
    else
    {
        latestSnapshot = findLatestStateSnapshot();
        std::cout << "[Runner] [" << cycleStamp.toStdString() << "] "
                  << "loading forward snapshot: "
                  << (latestSnapshot.isEmpty()
                          ? std::string("(cold start)")
                          : latestSnapshot.toStdString()) << "\n";
    }

    QString initialModelJsonPath;

    if (!latestSnapshot.isEmpty())
    {
        QJsonObject prevState = readJson(latestSnapshot);
        if (prevState.isEmpty())
        {
            std::cerr << "[Runner] Failed to read previous state: "
                      << latestSnapshot.toStdString() << "\n";
            return false;
        }

        // We will re-patch the simulation window inside runStage() per stage,
        // but writing the prev state once gives both stages a stable base file.
        initialModelJsonPath =
            QString::fromStdString(m_config.stateDir) + "/_current_input.json";
        if (!writeJson(prevState, initialModelJsonPath))
        {
            std::cerr << "[Runner] Failed to write base initial-condition state\n";
            return false;
        }
        std::cout << "[Runner] Initial condition: " << latestSnapshot.toStdString() << "\n";
    }
    else
    {
        std::cout << "[Runner] Initial condition: cold start from script "
                  << m_config.scriptFile << "\n";
        // initialModelJsonPath stays empty → runStage() cold-starts from script
    }

    // ------------------------------------------------------------------
    // Stage A — Advance [t, t+Δ]
    // ------------------------------------------------------------------
    const StageResult advance = runStage(StageKind::Advance,
                                         advanceStart, advanceEnd,
                                         initialModelJsonPath);
    if (!advance.ok)
    {
        std::cerr << "[Runner] Advance stage failed.\n";
        return false;
    }

    // Hand the freshly-written state snapshot to the assimilation thread
    // so its next calibration cycle has a System to load. The connection
    // is QueuedConnection across threads (see DTRunner::init()), so this
    // emit returns immediately and the assim thread picks the path up
    // through its own event loop. No-op when assimilation is disabled.
    if (m_assimilation && !advance.stateSnapshotPath.isEmpty())
    {
        emit snapshotReady(advance.stateSnapshotPath);
    }

    // ------------------------------------------------------------------
    // Stage B — Forecast [t, t+Δ+H]   (optional)
    // ------------------------------------------------------------------
    StageResult forecast;
    forecast.ok = false;

    if (m_forecastDays > 0.0)
    {
        const QDateTime forecastEnd =
            advanceStart.addMSecs(m_config.intervalMs + m_config.forecastHorizonMs);

        forecast = runStage(StageKind::Forecast,
                            advanceStart, forecastEnd,
                            initialModelJsonPath);
        if (!forecast.ok)
        {
            std::cerr << "[Runner] Forecast stage failed (continuing).\n";
            // Non-fatal: Advance already updated state for next cycle.
        }
    }
    else
    {
        std::cout << "[Runner] Forecast stage disabled (forecast_horizon = 0).\n";
    }

    // ------------------------------------------------------------------
    // Merge Advance + Forecast observed outputs into selected_output.csv
    //   - actuals: Advance series  [t, t+Δ]
    //   - forecast tail: Forecast series with t > t+Δ
    //   - knockout at (t - ε) drops the previous cycle's forecast tail
    // If Forecast is disabled or failed, only the Advance rows are merged
    // (and the file behaves as a pure actuals stream).
    // ------------------------------------------------------------------
    {
        const double cutoffSerial = toOHQDaySerial(advanceStart);

        // ------------------------------------------------------------
        // Source selection: when a forecast ran, it spans [t, t+1+H] and
        // already contains the Advance window [t, t+1] as its first day.
        // Use the forecast block as the single source for the CSV and skip
        // the Advance contribution (the overlapping day is identical by
        // construction). Fall back to Advance-only when forecast is
        // disabled or failed.
        // ------------------------------------------------------------
        const bool useForecastBlock = forecast.ok && (m_forecastDays > 0.0);

        TimeSeriesSet<double> primary =
            useForecastBlock ? forecast.observed : advance.observed;

        // ------------------------------------------------------------
        // Step 0: OHQ's observed-output series record the integration's
        //         t=0 sample as 0 (initial-state placeholder before the
        //         first solver step). Left in place this produces a
        //         visible "half-value" artifact at every cycle boundary
        //         after make_uniform interpolates from 0 up to the first
        //         real value. Replace each series' first sample with its
        //         second sample so make_uniform interpolates between two
        //         comparable values. No-op if a series has fewer than
        //         two points.
        // ------------------------------------------------------------
        for (size_t s = 0; s < primary.size(); ++s)
        {
            TimeSeries<double> &ts = primary[s];
            if (ts.size() >= 2)
                ts.setValue(0, ts.getValue(1));
        }

        // ------------------------------------------------------------
        // Step 1: resample observed outputs to the configured observation
        //         cadence (m_obsSaveIntervalDays).
        // ------------------------------------------------------------
        TimeSeriesSet<double> primaryProcessed =
            primary.make_uniform(m_obsSaveIntervalDays);

        // ------------------------------------------------------------
        // Step 2: apply correlated multiplicative log-normal noise using
        //         an OU process whose state persists across cycles
        //         (m_ouState). No-op if sigma == 0.
        // ------------------------------------------------------------
        if (m_config.observations.noiseSigma > 0.0 ||
            !m_config.observations.noiseSigmaByPattern.empty())
        {
            const double tauDays =
                static_cast<double>(m_config.observations.noiseCorrelationTimeMs)
                / (86400.0 * 1000.0);
            applyOUNoiseStateful(primaryProcessed,
                                 m_config.observations.noiseSigma,
                                 tauDays);
        }

        // ------------------------------------------------------------
        // Step 3: single block in, empty forecast tail. The merge knocks
        //         out the previous cycle's block at (advanceStart - eps)
        //         and writes this cycle's full [t, t+1+H] block fresh.
        // ------------------------------------------------------------
        TimeSeriesSet<double> emptyForecast;

        if (!mergeIntoSelectedOutput(primaryProcessed, emptyForecast, cutoffSerial))
            std::cerr << "[Runner] selected_output.csv merge failed (continuing).\n";

        // ------------------------------------------------------------
        // Step 4: refresh the metadata sidecar (cheap; describes the
        //         noise model that produced selected_output.csv).
        // ------------------------------------------------------------
        if (!writeMetadataSidecar())
            std::cerr << "[Runner] selected_output_meta.json write failed (continuing).\n";
    }
    // ------------------------------------------------------------------
    // Advance timing for next cycle
    // ------------------------------------------------------------------
    ++m_runsCompleted;
    m_nextIntervalStart = advanceEnd;

    const QString endStamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    std::cout << "[Runner] [" << endStamp.toStdString() << "] "
              << "Cycle complete. Next cycle start: "
              << m_nextIntervalStart.toString(Qt::ISODate).toStdString() << "\n";

    // ------------------------------------------------------------------
    // Stop condition: configured stop_datetime reached
    // ------------------------------------------------------------------
    if (!m_config.stopDatetime.empty())
    {
        const QDateTime stopAt = QDateTime::fromString(
            QString::fromStdString(m_config.stopDatetime), Qt::ISODate);
        if (stopAt.isValid() && m_nextIntervalStart >= stopAt)
        {
            std::cout << "[Runner] stop_datetime reached ("
                      << m_config.stopDatetime
                      << "). Requesting clean shutdown.\n";
            QCoreApplication::quit();
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// runStage
// Performs a single solve over [stageStart, stageEnd] starting from
// `modelJsonPath` (empty → cold start from script).
//
// Stage-dependent side effects:
//   Advance  : saves *_output.txt, viz_state.json, viz.svg, model snapshot,
//              state snapshot in stateDir, and (for now) appends the observed
//              outputs to selected_output.csv.
//   Forecast : writes only *_forecast_output.txt and forecast_viz.svg.
// ---------------------------------------------------------------------------
StageResult DTRunner::runStage(StageKind kind,
                               const QDateTime &stageStart,
                               const QDateTime &stageEnd,
                               const QString &modelJsonPath)
{
    const QDateTime wallStart = QDateTime::currentDateTimeUtc();
    const double simStartSerial = toOHQDaySerial(stageStart);
    const double simEndSerial   = toOHQDaySerial(stageEnd);
    const QString initialIcPath = modelJsonPath.isEmpty()
                                      ? QStringLiteral("(cold start from script)")
                                      : modelJsonPath;

    StageResult result;
    result.kind = kind;

    const bool isAdvance = (kind == StageKind::Advance);
    const char *stageLabel = isAdvance ? "Advance" : "Forecast";

    const double ohqStart = toOHQDaySerial(stageStart);
    const double ohqEnd   = toOHQDaySerial(stageEnd);

    std::cout << "\n[Runner] ---- Stage " << stageLabel << " ----\n"
              << "[Runner] Wall clock : "
              << stageStart.toString(Qt::ISODate).toStdString() << "  →  "
              << stageEnd.toString(Qt::ISODate).toStdString() << "\n"
              << "[Runner] OHQ serial : " << ohqStart << "  →  " << ohqEnd << "\n";

    // ------------------------------------------------------------------
    // Patch the simulation window in the initial-condition file (if any).
    // Each stage writes its own patched copy so the two solves don't
    // race over the same _current_input.json.
    // ------------------------------------------------------------------
    QString patchedPath;
    if (!modelJsonPath.isEmpty())
    {
        const QJsonObject base    = readJson(modelJsonPath);
        const QJsonObject patched = patchSimulationWindow(base, ohqStart, ohqEnd);

        const QString suffix = isAdvance ? "_advance.json" : "_forecast.json";
        patchedPath = QString::fromStdString(m_config.stateDir) +
                      "/_current_input" + suffix;
        if (!writeJson(patched, patchedPath))
        {
            std::cerr << "[Runner] Failed to write patched state for "
                      << stageLabel << "\n";
            return result;
        }
    }

    // ------------------------------------------------------------------
    // Build and solve
    // ------------------------------------------------------------------
    const std::string defaultTemplatePath =
        QCoreApplication::applicationDirPath().toStdString() + "/../../resources/";
    const std::string settingsFile = defaultTemplatePath + "settings.json";

    std::unique_ptr<System> ohqSystem(new System());
    ohqSystem->SetDefaultTemplatePath(defaultTemplatePath);
    ohqSystem->SetWorkingFolder(
        QString::fromStdString(m_config.outputDir).toStdString() + "/");

    std::cout << "[Runner] Preparing model...\n";
    if (!patchedPath.isEmpty())
    {
        ohqSystem->ReadSystemSettingsTemplate(settingsFile);
        ohqSystem->LoadfromJson(patchedPath);
    }
    else
    {
        // Cold start
        Script scr(m_config.scriptFile, ohqSystem.get());
        ohqSystem->CreateFromScript(scr, settingsFile);
        ohqSystem->SetProp("simulation_start_time", ohqStart);
        ohqSystem->SetProp("simulation_end_time",   ohqEnd);

    }

    ohqSystem->SetSilent(false);


    // Precipitation (clamped to this stage's window inside fetchPrecipitation)
    CPrecipitation precip = DTWeather::fetchPrecipitation(
        m_config.weatherSource,
        m_config.latitude,
        m_config.longitude,
        stageStart, stageEnd);

    {
        if (m_config.keepDebugOutputs)
        {
            const QString precipFile =
                QString::fromStdString(m_config.outputDir) + "/" +
                stageStart.toString("yyyyMMdd_HHmmss") + "_" +
                (isAdvance ? "advance" : "forecast") + "_precipitation.txt";
            precip.writefile(precipFile.toStdString());
        }
    }
    DTWeather::injectPrecipitation(ohqSystem.get(), precip);

    // Apply Truth Twin parameter drift (no-op if config has no entries).
    // Query each drift series at the midpoint of this stage's window so the
    // applied value is representative of the interval being solved.
    const double driftQueryTime = 0.5 * (simStartSerial + simEndSerial);
    applyParameterDrift(ohqSystem.get(), driftQueryTime);

    // Penman ET forcings — fetch and inject the four weather variables that
    // the "Evapotranspiration_Penman (Soil)" source consumes. If the model
    // uses a different ET formulation, the missing-variable warnings from
    // injectWeather are harmless.
    const std::string etSource = "Evapotranspiration_Penman (Soil)";

    const auto temp = DTWeather::fetchWeatherVariable(
        m_config.weatherSource, "temperature_2m",
        m_config.latitude, m_config.longitude, stageStart, stageEnd);
    DTWeather::injectWeather(ohqSystem.get(), etSource, "Temperature", temp);

    const auto rh = DTWeather::fetchWeatherVariable(
                        m_config.weatherSource, "relative_humidity_2m",
                        m_config.latitude, m_config.longitude, stageStart, stageEnd)/100.00;
    DTWeather::injectWeather(ohqSystem.get(), etSource, "R_h", rh);

    const auto wind = DTWeather::fetchWeatherVariable(
        m_config.weatherSource, "windspeed_10m",
        m_config.latitude, m_config.longitude, stageStart, stageEnd);
    DTWeather::injectWeather(ohqSystem.get(), etSource, "wind_speed", wind);

    const auto rad = DTWeather::fetchWeatherVariable(
        m_config.weatherSource, "shortwave_radiation",
        m_config.latitude, m_config.longitude, stageStart, stageEnd);
    DTWeather::injectWeather(ohqSystem.get(), etSource, "solar_radiation", rad);


    ohqSystem->CalcAllInitialValues();
    std::cout << "[Runner] Solving...\n";
    ohqSystem->Solve(true);

    // ------------------------------------------------------------------
    // Outputs
    // ------------------------------------------------------------------
    const QString stageTag = isAdvance ? "_output.txt" : "_forecast_output.txt";
    const QString outputFile =
        QString::fromStdString(m_config.outputDir) + "/" +
        stageStart.toString("yyyyMMdd_HHmmss") + stageTag;

    if (m_config.keepDebugOutputs)
    {
        std::cout << "[Runner] Writing output to: " << outputFile.toStdString() << "\n";
        ohqSystem->GetObservedOutputs().write(outputFile.toStdString());
        result.outputFilePath = outputFile;
    }
    else
    {
        result.outputFilePath.clear();   // not written this cycle
    }

    // Capture the observed outputs for the (future) merge step.
    result.observed = ohqSystem->GetObservedOutputs();

    // ------------------------------------------------------------------
    // Stage-specific side effects
    // ------------------------------------------------------------------
    if (isAdvance)
    {
        // Export configured state variables
        for (const auto &exp : m_config.stateVarExports)
            ohqSystem->SaveStateVariableToJson(exp.variable, exp.outputPath);

        // Save model snapshot (drives next cycle, and is read by the
        // assimilation loop via setLatestSnapshot()). In debug mode each
        // cycle is timestamped and archived; otherwise we overwrite a
        // single fixed file to keep snapshots/ bounded on long runs.
        const QString modelSnapshotPath = m_config.keepDebugOutputs
                                              ? (QString::fromStdString(m_config.modelSnapshotDir) + "/" +
                                                 stageStart.toString("yyyyMMdd_HHmmss") + "_model.json")
                                              : (QString::fromStdString(m_config.modelSnapshotDir) +
                                                 "/_latest_model.json");
        ohqSystem->SavetoJson(modelSnapshotPath.toStdString(),
                              ohqSystem->addedtemplates, false, true);

        // Full state for visualization
        const QString vizStatePath =
            QString::fromStdString(m_config.outputDir) + "/viz_state.json";
        ohqSystem->SaveFullStateTo(vizStatePath);
        std::cout << "[Runner] Viz state written to: "
                  << vizStatePath.toStdString() << "\n";

        const QString vizJsonPath = QString::fromStdString(m_config.vizFile);
        const QString vizSvgPath  =
            QString::fromStdString(m_config.outputDir) + "/viz.svg";
        const QJsonObject fullState = readJson(vizStatePath);
        QString vizErr;
        if (!VizRenderer::render(vizJsonPath, fullState, vizSvgPath, vizErr))
            std::cerr << "[Runner] VizRenderer warning: " << vizErr.toStdString() << "\n";
        else
            std::cout << "[Runner] viz.svg written to: " << vizSvgPath.toStdString() << "\n";
        result.vizSvgPath = vizSvgPath;

        // State snapshot in stateDir (this is what next cycle picks up)
        QJsonObject savedState = readJson(modelSnapshotPath);
        savedState["_dt_interval_start_utc"] = stageStart.toString(Qt::ISODate);
        savedState["_dt_interval_end_utc"]   = stageEnd.toString(Qt::ISODate);
        savedState["_dt_next_start_utc"]     = stageEnd.toString(Qt::ISODate);
        savedState["_dt_runs_completed"]     = m_runsCompleted + 1;
        savedState["_dt_output_file"]        =
            m_config.keepDebugOutputs ? outputFile : QString();

        const QString snapshotPath =
            QString::fromStdString(m_config.stateDir) + "/" +
            makeSnapshotFilename(stageEnd);
        if (!writeJson(savedState, snapshotPath))
            std::cerr << "[Runner] Warning: failed to write state snapshot\n";
        else
            std::cout << "[Runner] State snapshot: " << snapshotPath.toStdString() << "\n";
        result.stateSnapshotPath = snapshotPath;
    }
    else // Forecast
    {
        // Forecast viz: independent SVG, generated from forecast end-state.
        // No model snapshot, no state snapshot, no state-variable exports.
        const QString forecastVizStatePath =
            QString::fromStdString(m_config.outputDir) + "/forecast_viz_state.json";
        ohqSystem->SaveFullStateTo(forecastVizStatePath);

        const QString vizJsonPath = QString::fromStdString(m_config.vizFile);
        const QString forecastVizSvgPath =
            QString::fromStdString(m_config.outputDir) + "/forecast_viz.svg";
        const QJsonObject fullState = readJson(forecastVizStatePath);
        QString vizErr;
        if (!VizRenderer::render(vizJsonPath, fullState, forecastVizSvgPath, vizErr))
            std::cerr << "[Runner] Forecast VizRenderer warning: "
                      << vizErr.toStdString() << "\n";
        else
            std::cout << "[Runner] forecast_viz.svg written to: "
                      << forecastVizSvgPath.toStdString() << "\n";
        result.vizSvgPath = forecastVizSvgPath;
    }

    result.ok = true;

    if (m_runLogger)
    {
        m_runLogger->recordRun(
            (kind == StageKind::Advance)
                ? RunLogger::RunType::ForwardAdvance
                : RunLogger::RunType::ForwardForecast,
            m_runsCompleted + 1,                      // cycle (current)
            wallStart,
            QDateTime::currentDateTimeUtc(),
            simStartSerial,
            simEndSerial,
            initialIcPath,
            result.stateSnapshotPath,                  // empty for Forecast
            result.ok ? RunLogger::Status::Ok : RunLogger::Status::Failed,
            result.ok ? QString()
                      : QStringLiteral("runStage failed; see runner log"));
    }
    return result;

    return result;
}

// ---------------------------------------------------------------------------
// mergeIntoSelectedOutput
// Maintains selected_output.csv as:
//     [actuals up to advanceEnd] + [forecast tail (advanceEnd, advanceEnd+H]]
//
// Each cycle:
//   1. Load existing CSV (if any) into a TimeSeriesSet.
//   2. Knockout at (advanceStart - epsilon) to drop the previous cycle's
//      forecast tail (which started at the previous advanceStart and now
//      should be replaced by today's authoritative actuals + new forecast).
//   3. Append the Advance observed rows ([advanceStart, advanceEnd]).
//   4. Append the Forecast observed rows whose t > advanceEnd
//      (i.e. the forecast tail beyond the just-added Advance window).
//   5. Write back with full header.
//
// Series are matched by name across loaded / advance / forecast sets.
// ---------------------------------------------------------------------------
bool DTRunner::mergeIntoSelectedOutput(const TimeSeriesSet<double> &advanceObs,
                                       const TimeSeriesSet<double> &forecastObs,
                                       double cutoffTime)
{
    const QString selectedOutputFile =
        QString::fromStdString(m_config.outputDir) + "/selected_output.csv";

    // Tiny epsilon in day units to drop the boundary point from the
    // previous cycle so today's Advance can re-supply it without duplication.
    // ~0.5 ms in days = 5.787e-9. We use 1e-6 (≈86 ms) to be safely above
    // any float round-trip in CSV write/read cycles.
    constexpr double kBoundaryEpsilonDays = 1e-6;

    // ------------------------------------------------------------------
    // 1. Load existing file (or start fresh)
    // ------------------------------------------------------------------
    TimeSeriesSet<double> merged;
    QFileInfo fi(selectedOutputFile);
    if (fi.exists() && fi.size() > 0)
    {
        TimeSeriesSet<double> loaded(selectedOutputFile.toStdString(), true);
        if (!loaded.file_not_found) {
            merged = loaded;
            // DIAG: what did we actually read back?
            if (!merged.empty()) {
                const auto &ts = merged[0];
                std::cerr << "[DIAG] loaded '" << ts.name() << "' size=" << ts.size()
                          << " t[0]=" << (ts.size()? ts.getTime(0):-1)
                          << " t[last]=" << (ts.size()? ts.getTime(ts.size()-1):-1)
                          << " cutoff=" << (cutoffTime - kBoundaryEpsilonDays) << "\n";
            }
            const size_t before = merged.empty()?0:merged[0].size();

            std::cerr << "[DIAG2] elem[0].t=" << merged[0][0].t
                      << " getTime(0)=" << merged[0].getTime(0)
                      << " | elem[last].t=" << merged[0][merged[0].size()-1].t
                      << " getTime(last)=" << merged[0].getTime(merged[0].size()-1) << "\n";

            merged.knockout(cutoffTime - kBoundaryEpsilonDays);
            const size_t after = merged.empty()?0:merged[0].size();
            std::cerr << "[DIAG] knockout dropped " << (before-after)
                      << " points (before=" << before << " after=" << after << ")\n";
        }
        if (loaded.file_not_found)
        {
            std::cerr << "[Runner] mergeIntoSelectedOutput: failed to read "
                      << selectedOutputFile.toStdString()
                      << " — starting fresh.\n";
        }
        else
        {
            merged = loaded;
            // 2. Drop the previous cycle's forecast tail.
            merged.knockout(cutoffTime - kBoundaryEpsilonDays);
        }
    }
    else
    {
        // Bootstrap: take the structure (series + names) from advanceObs.
        // No rows yet; they get added in step 3.
        for (size_t i = 0; i < advanceObs.size(); ++i)
        {
            TimeSeries<double> empty;
            empty.setName(advanceObs[i].name());
            merged.push_back(empty);
        }
    }

    // Helper: find a series in `merged` by name; create it if missing.
    auto findOrCreate = [&merged](const std::string &name) -> TimeSeries<double>& {
        for (size_t i = 0; i < merged.size(); ++i)
            if (merged[i].name() == name)
                return merged[i];
        TimeSeries<double> empty;
        empty.setName(name);
        merged.push_back(empty);
        return merged.back();
    };

    // ------------------------------------------------------------------
    // 3. Append Advance observed rows (the new actuals).
    // ------------------------------------------------------------------
    for (size_t i = 0; i < advanceObs.size(); ++i)
    {
        const TimeSeries<double> &src = advanceObs[i];
        TimeSeries<double> &dst = findOrCreate(src.name());
        for (size_t j = 0; j < src.size(); ++j)
        {
            const auto &pt = src[j];
            dst.append(pt.t, pt.c);
        }
    }

    // ------------------------------------------------------------------
    // 4. Append Forecast observed rows beyond advanceEnd (the forecast tail).
    //    cutoffTime here is advanceStart, not advanceEnd — we need the
    //    Advance window's *end* to slice the forecast.
    //    The orchestrator passes advanceEnd as a separate argument… except
    //    we only have cutoffTime in this signature. We derive advanceEnd
    //    by taking the last Advance timestamp seen above.
    // ------------------------------------------------------------------
    double advanceLastT = cutoffTime;  // fallback if Advance is empty
    if (!advanceObs.empty())
    {
        for (size_t i = 0; i < advanceObs.size(); ++i)
        {
            const TimeSeries<double> &src = advanceObs[i];
            if (!src.empty())
                advanceLastT = std::max(advanceLastT, src[src.size() - 1].t);
        }
    }

    for (size_t i = 0; i < forecastObs.size(); ++i)
    {
        const TimeSeries<double> &src = forecastObs[i];
        TimeSeries<double> &dst = findOrCreate(src.name());
        for (size_t j = 0; j < src.size(); ++j)
        {
            const auto &pt = src[j];
            if (pt.t > advanceLastT)
                dst.append(pt.t, pt.c);
        }
    }

    // ------------------------------------------------------------------
    // 5. Reorder and write back (full file with header)
    // ------------------------------------------------------------------
    merged = dtReorderSelectedOutput(merged, m_config.deploymentName);
    merged.write(selectedOutputFile.toStdString());
    std::cout << "[Runner] selected_output.csv merged: "
              << merged.size() << " series, "
              << merged.maxnumpoints() << " max rows\n";

    for (size_t i = 0; i < merged.size(); ++i) {
        const auto &ts = merged[i];
        for (size_t j = 1; j < ts.size(); ++j) {
            if (ts.getTime(j) < ts.getTime(j-1)) {
                std::cerr << "[Runner] NON-MONOTONIC in '" << ts.name()
                << "' at idx " << j << ": " << ts.getTime(j-1)
                << " -> " << ts.getTime(j)
                << " (advanceStart=" << cutoffTime << ")\n";
                break;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// findLatestStateSnapshot
// Returns the full path of the most-recent state_YYYYMMDD_HHmmss.json file.
// ---------------------------------------------------------------------------
QString DTRunner::findLatestStateSnapshot() const
{
    const QDir dir(QString::fromStdString(m_config.stateDir));
    if (!dir.exists()) return {};

    const QStringList files = dir.entryList(
        QStringList() << "state_????????_??????.json",
        QDir::Files, QDir::Name | QDir::Reversed); // Reversed → newest first

    if (files.isEmpty()) return {};
    return dir.absoluteFilePath(files.first());
}

// ---------------------------------------------------------------------------
// makeSnapshotFilename
// ---------------------------------------------------------------------------
QString DTRunner::makeSnapshotFilename(const QDateTime &dt) const
{
    return "state_" + dt.toUTC().toString("yyyyMMdd_HHmmss") + ".json";
}

// ---------------------------------------------------------------------------
// toOHQDaySerial
// Days since 1899-12-30 (Excel epoch), with fractional day for time-of-day.
// ---------------------------------------------------------------------------
double DTRunner::toOHQDaySerial(const QDateTime &dt)
{
    const qint64 msSinceEpoch =
        QDateTime(kOHQEpoch, QTime(0, 0, 0), Qt::UTC).msecsTo(dt.toUTC());
    return static_cast<double>(msSinceEpoch) / (86400.0 * 1000.0);
}

// ---------------------------------------------------------------------------
// fromOHQDaySerial
// ---------------------------------------------------------------------------
QDateTime DTRunner::fromOHQDaySerial(double serial)
{
    const qint64 msOffset = static_cast<qint64>(serial * 86400.0 * 1000.0);
    return QDateTime(kOHQEpoch, QTime(0, 0, 0), Qt::UTC).addMSecs(msOffset);
}

// ---------------------------------------------------------------------------
// writeJson
// ---------------------------------------------------------------------------
bool DTRunner::writeJson(const QJsonObject &obj, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        std::cerr << "[Runner] Cannot write: " << path.toStdString() << "\n";
        return false;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return true;
}

// ---------------------------------------------------------------------------
// readJson
// ---------------------------------------------------------------------------
QJsonObject DTRunner::readJson(const QString &path)
{
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return {};
    const auto doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

// ---------------------------------------------------------------------------
// patchSimulationWindow
// Updates simulation_start_time and simulation_end_time inside the
// Settings sub-object of an OHQ state JSON.
// ---------------------------------------------------------------------------
QJsonObject DTRunner::patchSimulationWindow(const QJsonObject &state,
                                            double startSerial,
                                            double endSerial)
{
    QJsonObject patched = state;

    QJsonObject settings = patched.value("Settings").toObject();
    settings["simulation_start_time"] = QString::number(startSerial, 'f', 6);
    settings["simulation_end_time"]   = QString::number(endSerial,   'f', 6);
    patched["Settings"] = settings;

    return patched;
}

// ---------------------------------------------------------------------------
// fetchPrecipitation
// Fetches precipitation for [intervalStart, intervalEnd] from the source
// configured in config.weather_source:
//   "openmeteo"            -> 7-day forecast (real-time / forward mode)
//   "openmeteo_historical" -> ERA5 archive   (Truth-Twin / replay mode)
// Returns CPrecipitation with bins in OHQ day-serial units, in metres.
// ---------------------------------------------------------------------------
CPrecipitation DTRunner::fetchPrecipitation(const QDateTime &intervalStart,
                                            const QDateTime &intervalEnd)
{
    NOAAWeatherFetcher fetcher;

    if (m_config.weatherSource == "openmeteo") {
        CPrecipitation precip = fetcher.getOpenMeteoPrecipitation(
            m_config.latitude, m_config.longitude,
            intervalStart, intervalEnd);
        if (precip.n == 0)
            std::cerr << "[Runner] Warning: " << fetcher.lastError().toStdString() << "\n";
        return precip;
    }

    if (m_config.weatherSource == "openmeteo_historical") {
        CPrecipitation precip = fetcher.getOpenMeteoHistoricalPrecipitation(
            m_config.latitude, m_config.longitude,
            intervalStart, intervalEnd);
        if (precip.n == 0)
            std::cerr << "[Runner] Warning: " << fetcher.lastError().toStdString() << "\n";
        return precip;
    }

    std::cerr << "[Runner] Unknown weather_source: '"
              << m_config.weatherSource << "' (expected 'openmeteo' or "
              << "'openmeteo_historical')\n";
    return CPrecipitation();
}

void DTRunner::injectPrecipitation(System *system, const CPrecipitation &precip)
{
    if (precip.n == 0) {
        std::cerr << "[Runner] injectPrecipitation: empty precipitation, skipping.\n";
        return;
    }

    // The block name in the .ohq is "Rain" (capitalized) — match that exactly.
    Source *src = system->source("Rain");
    if (!src) {
        std::cerr << "[Runner] injectPrecipitation: source 'Rain' not found in system.\n";
        return;
    }

    auto *var = src->Variable("timeseries");
    if (!var) {
        std::cerr << "[Runner] injectPrecipitation: variable 'timeseries' not found on source 'Rain'.\n";
        return;
    }

    var->SetTimeSeries(precip);
    std::cout << "[Runner] Injected " << precip.n
              << " precipitation bins into 'Rain' source.\n";

    auto *intensityVar = src->Variable("timeseries");
    if (intensityVar) {
        std::cout << "[Inject DIAG] After SetTimeSeries on Rain.timeseries:\n";
        std::cout << "  Rain.timeseries._timeseries.size() = "
                  << var->GetTimeSeries()->size() << "\n";   // or whatever the getter is
        std::cout << "  Rain.Intensity._val = "
                  << intensityVar->GetVal() << "\n";  // should be 0 here, that's fine
        std::cout << "  Rain.Intensity expression = "
                  << intensityVar->GetExpression()->ToString() << "\n";
    }
    std::cout << "[Runner] Injected " << precip.n << " precipitation bins...\n";
}

// ---------------------------------------------------------------------------
// applyParameterDrift
//
// Truth Twin parameter drift injection. For each (parameter, TimeSeries)
// configured in m_config.parameterDrift, interpolate the series at
// tSerial (OHQ day-serial) and write the value into the live System via
// Parameter::SetValue(). Called once per stage, after the System is
// built/loaded and before CalcAllInitialValues()/Solve(), so the
// drifted value participates in this cycle's solve and is automatically
// captured in the state snapshot written at the end of the stage.
//
// No-op when m_parameterDrift is empty. Missing parameters log a warning
// but do not abort the cycle — drift is auxiliary forcing.
// ---------------------------------------------------------------------------
void DTRunner::applyParameterDrift(System *system, double tSerial)
{
    if (m_parameterDrift.empty()) return;
    if (!system) return;

    for (size_t i = 0; i < m_parameterDrift.size(); ++i)
    {
        const std::string &name = m_config.parameterDrift[i].parameter;
        const double value = m_parameterDrift[i].interpol(tSerial);

        Parameter *p = system->GetParameter(name);
        if (!p)
        {
            std::cerr << "[Runner] Parameter drift: parameter '"
                      << name << "' not found in system; skipping.\n";
            continue;
        }
        p->SetValue(value);
        std::cout << "[Runner] Parameter drift: " << name
                  << " <- " << value
                  << " (at t=" << tSerial << ")\n";
    }
}


// ---------------------------------------------------------------------------
// applyOUNoiseStateful
//
// Apply multiplicative log-normal noise driven by a continuous Ornstein-
// Uhlenbeck process with unit stationary variance:
//
//     x_obs(t) = x_model(t) * exp(sigma * epsilon(t))
//
// where epsilon evolves according to dε = -ε/τ dt + √(2/τ) dW, integrated
// with the *exact* Gaussian transition (not Euler-Maruyama):
//
//     epsilon_{n+1} = phi * epsilon_n + sqrt(1 - phi^2) * eta_n
//     phi           = exp(-Δt / τ)
//     eta_n         ~ N(0, 1)
//
// The OU state is persisted in m_ouState across runOnce() cycles, keyed by
// series name, so correlation continues smoothly between cycles. First time
// a series name is seen, epsilon is drawn from N(0,1) (the stationary
// distribution).
//
// τ <= 0 is treated as the white-noise limit (each point gets an independent
// N(0,1) draw). σ <= 0 is a no-op.
// ---------------------------------------------------------------------------
double DTRunner::observationNoiseSigmaForSeries(const std::string &seriesName) const
{
    const std::string n = dtLowerCopy(seriesName);

    for (const auto &kv : m_config.observations.noiseSigmaByPattern)
    {
        if (!kv.first.empty() && n.find(kv.first) != std::string::npos)
            return kv.second;
    }

    return m_config.observations.noiseSigma;
}

// Per-series error shape, resolved by the same substring patterns as sigma.
NoiseMode DTRunner::observationNoiseModeForSeries(const std::string &seriesName) const
{
    const std::string n = dtLowerCopy(seriesName);
    for (const auto &kv : m_config.observations.noiseModeByPattern)
    {
        if (!kv.first.empty() && n.find(kv.first) != std::string::npos)
            return kv.second;
    }
    return m_config.observations.noiseMode;
}

void DTRunner::applyOUNoiseStateful(TimeSeriesSet<double> &set,
                                    double defaultSigma,
                                    double tauDays)
{
    if (defaultSigma <= 0.0 && m_config.observations.noiseSigmaByPattern.empty())
        return;
    if (set.empty()) return;

    // One shared RNG for the whole process. random_device seeds it once.
    // For reproducible Truth Twins, swap in a fixed seed (e.g. from
    // m_config.observations.seed if you add such a field) here.
    static thread_local std::mt19937 gen{ std::random_device{}() };
    static thread_local std::normal_distribution<double> N01{ 0.0, 1.0 };

    for (size_t s = 0; s < set.size(); ++s)
    {
        TimeSeries<double> &ts = set[s];
        if (ts.empty()) continue;

        // Name-indexed: robust against OHQ reordering observed outputs.
        const std::string seriesKey = ts.name();
        const double seriesSigma = observationNoiseSigmaForSeries(seriesKey);
        if (seriesSigma <= 0.0) continue;
        const NoiseMode seriesMode = observationNoiseModeForSeries(seriesKey);

        // Initialize or fetch the persisted OU state for this series.
        double eps;
        auto it = m_ouState.find(seriesKey);
        if (it == m_ouState.end())
        {
            eps = N01(gen);                  // stationary draw
            m_ouState[seriesKey] = eps;
        }
        else
        {
            eps = it->second;
        }

        // Walk the series, advancing OU exactly between samples.
        for (size_t i = 0; i < ts.size(); ++i)
        {
            if (i > 0)
            {
                const double dt = ts.getTime(i) - ts.getTime(i - 1);

                if (tauDays <= 0.0 || dt <= 0.0)
                {
                    // White-noise limit / degenerate dt: independent draw.
                    eps = N01(gen);
                }
                else
                {
                    const double phi = std::exp(-dt / tauDays);
                    const double sqrt_one_minus_phi2 =
                        std::sqrt(std::max(0.0, 1.0 - phi * phi));
                    eps = phi * eps + sqrt_one_minus_phi2 * N01(gen);
                }
            }

            const double clean = ts.getValue(i);
            // Additive keeps sigma in the observation's own units, so the
            // injected error does not vanish as the quantity goes to zero.
            // Small negative readings during dry periods are retained
            // deliberately: an ultrasonic sensor ranging to a dry pond bottom
            // genuinely reports 0 +/- its accuracy, and clipping at zero would
            // reintroduce the very bias this mode exists to remove.
            const double noised =
                (seriesMode == NoiseMode::Additive)
                    ? clean + seriesSigma * eps
                    : clean * std::exp(seriesSigma * eps);
            ts.setValue(i, noised);
        }

        // Persist epsilon for the next cycle.
        m_ouState[seriesKey] = eps;
    }
}

// ---------------------------------------------------------------------------
// writeMetadataSidecar
//
// Writes outputs/selected_output_meta.json describing the noise model
// applied to selected_output.csv. Idempotent: re-written every cycle so
// `last_updated_utc` reflects the most recent merge.
// ---------------------------------------------------------------------------
bool DTRunner::writeMetadataSidecar() const
{
    QJsonObject obsBlock;
    obsBlock["save_interval_ms"] =
        static_cast<qint64>(m_config.observations.saveIntervalMs);
    obsBlock["save_interval_days"] = m_obsSaveIntervalDays;
    if (m_config.observations.noiseSigmaByPattern.empty())
    {
        obsBlock["noise_sigma"] = m_config.observations.noiseSigma;
    }
    else
    {
        QJsonObject sigmaObj;
        sigmaObj["default"] = m_config.observations.noiseSigma;
        for (const auto &kv : m_config.observations.noiseSigmaByPattern)
            sigmaObj[QString::fromStdString(kv.first)] = kv.second;
        obsBlock["noise_sigma"] = sigmaObj;
    }
    obsBlock["noise_correlation_time_ms"] =
        static_cast<qint64>(m_config.observations.noiseCorrelationTimeMs);
    obsBlock["noise_correlation_time_days"] =
        static_cast<double>(m_config.observations.noiseCorrelationTimeMs)
        / (86400.0 * 1000.0);
    obsBlock["noise_model"] =
        (m_config.observations.noiseMode == NoiseMode::Additive)
            ? QStringLiteral("additive_gaussian_OU  x_obs = x_model + sigma * epsilon")
            : QStringLiteral("multiplicative_log_normal_OU "
                             "x_obs = x_model * exp(sigma * epsilon)");
    if (!m_config.observations.noiseModeByPattern.empty())
    {
        QJsonObject modes;
        for (const auto &kv : m_config.observations.noiseModeByPattern)
            modes[QString::fromStdString(kv.first)] =
                (kv.second == NoiseMode::Additive) ? "additive" : "multiplicative";
        obsBlock["noise_mode_by_pattern"] = modes;
    }
    obsBlock["ou_stationary_variance"] = 1.0;

    QJsonObject root;
    root["observations"]    = obsBlock;
    root["deployment_name"] = QString::fromStdString(m_config.deploymentName);
    root["runs_completed"]  = m_runsCompleted;
    root["last_updated_utc"] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    const QString path =
        QString::fromStdString(m_config.outputDir) + "/selected_output_meta.json";

    return writeJson(root, path);
}

void DTRunner::onCalibrationCompleted(QString newSnapshotPath)
{
    const QString stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    m_pendingCalibratedSnapshot = newSnapshotPath;
    std::cout << "[Runner] [" << stamp.toStdString() << "] "
              << "calibrated snapshot received: "
              << newSnapshotPath.toStdString() << "\n"
              << "[Runner] (will consume on next forward cycle)\n";
}

QDateTime fromOHQDaySerial(double serial)
{
    const qint64 ms = static_cast<qint64>((serial - 25569.0) * 86400000.0);
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC);
}
