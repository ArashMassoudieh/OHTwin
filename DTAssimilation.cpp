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

#include "DTAssimilation.h"
#include "DTConfig.h"
#include "System.h"
#include "Script.h"
#include "GA.h"
#include "Object.h"
#include "ErrorHandler.h"
#include "observation.h"
#include "Quan.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QCoreApplication>
#include "DTWeather.h"
#include <QDateTime>
#include <iostream>
#include <QThread>
#include "RunLogger.h"
#include "DTStreamingMCMC.h"
#include "DTDebugLog.h"

DTAssimilation::DTAssimilation(const DTConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
    m_pollTimer.setParent(this);
    m_pollTimer.setSingleShot(false);
    // The timer is parented to *this* (it's a value member). When this
    // object is moveToThread()'d to the assimilation thread, the timer
    // moves with it. startTimer() will then be called on that thread
    // via a queued connection from QThread::started.
    connect(&m_pollTimer, &QTimer::timeout,
            this,         &DTAssimilation::onPollTick);
}


// ---------------------------------------------------------------------------
// configure
// Validate config, install endpoints, perform the initial buffer refresh,
// and compute the wall-clock poll interval. Must run on the main thread
// before moveToThread().  Does NOT start the timer — startTimer() does
// that, and must run on the assimilation thread.
// ---------------------------------------------------------------------------
bool DTAssimilation::configure(QString &errorMessage)
{
    if (!m_config.assimilation.enabled)
    {
        errorMessage = "DTAssimilation::configure(): assimilation block is "
                       "disabled in config (this is a programmer error — "
                       "DTRunner shouldn't construct DTAssimilation when "
                       "the block is absent).";
        return false;
    }
    if (m_config.assimilation.truthCsvUrl.empty())
    {
        errorMessage = "DTAssimilation::configure(): truth_csv_url is empty";
        return false;
    }
    if (m_config.assimilation.pollIntervalMs <= 0)
    {
        errorMessage = "DTAssimilation::configure(): poll_interval must be > 0";
        return false;
    }

    m_buffer.setEndpoints(
        QString::fromStdString(m_config.assimilation.truthCsvUrl),
        QString::fromStdString(m_config.assimilation.truthMetaUrl));

    // Apply time_acceleration the same way the forward loop does, so that
    // poll_interval is interpreted in simulated time. With acceleration=1
    // this is plain wall-clock; with acceleration>1 the calibration timer
    // fires proportionally faster.
    const double wallClockIntervalMsD =
        static_cast<double>(m_config.assimilation.pollIntervalMs)
        / m_config.timeAcceleration;
    const qint64 wallClockIntervalMs =
        static_cast<qint64>(std::max(1.0, wallClockIntervalMsD));
    m_pollIntervalWallClockMs =
        std::min(wallClockIntervalMs, static_cast<qint64>(INT_MAX));

    // NOTE: the initial buffer refresh has been moved to startTimer(),
    // which runs on the assimilation thread. Refreshing here triggers
    //   "QObject::startTimer: Timers cannot be started from another thread"
    // because m_buffer's QNetworkAccessManager is constructed on the
    // assimilation thread (via moveToThread()) and cannot be used from
    // the main thread.
    return true;
}

void DTAssimilation::onPollTick()
{
    // Defensive guard. Under normal operation the Qt event loop on the
    // assimilation thread serializes timer ticks behind the running
    // calibration, so this flag is never observed true. It matters only
    // if runCalibration() ever pumps a local event loop (e.g. for
    // progress signals), in which case a re-entrant tick could otherwise
    // start a second calibration on top of the first.
    if (m_calibrationInProgress)
    {
        std::cout << "[Assim] poll tick skipped (calibration in progress)\n";
        return;
    }

    if (!m_buffer.refresh())
    {
        std::cerr << "[Assim] poll failed: "
                  << m_buffer.lastError().toStdString() << "\n";
        emit pollFailed(m_buffer.lastError());
        return;   // skip calibration on stale data
    }

    // Publish buffer summary for cross-thread readers (the forward loop
    // reads these atomics from the main thread when deciding the
    // advance window in runOnce()).
    m_bufferTMax.store(m_buffer.tMax());
    m_bufferPointCount.store(static_cast<qint64>(m_buffer.pointCount()));

    std::cout << "[Assim] poll OK — " << m_buffer.pointCount()
              << " points buffered, starting calibration\n";
    emit buffered(static_cast<qint64>(m_buffer.pointCount()));

    m_calibrationInProgress = true;
    QString calErr;
    bool ok = false;
    try {
        ok = runCalibration(calErr);
    } catch (const std::exception &e) {
        calErr = QString("unhandled exception in calibration: %1").arg(e.what());
        DTDebugLog::instance().log(DTDebugLog::Category::Assim, calErr);
        DTDebugLog::instance().flush();
        recordCalibrationFailure(QDateTime::currentDateTimeUtc(), calErr);
    } catch (...) {
        calErr = "unhandled non-std exception in calibration";
        DTDebugLog::instance().log(DTDebugLog::Category::Assim, calErr);
        DTDebugLog::instance().flush();
    }
    m_calibrationInProgress = false;

    if (!ok)
    {
        std::cerr << "[Assim] calibration failed: "
                  << calErr.toStdString() << "\n";
        emit calibrationFailed(calErr);
        return;
    }
}


// ---------------------------------------------------------------------------
// archiveGAOutput
// Append ga_output.txt to ga_output_merged.txt with a cycle-delimiter header.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// archiveGAOutput
//
// Purpose:
//   1. Append the current cycle's ga_output.txt to a cumulative
//      ga_output_merged.txt file.
//
//   2. Optionally preserve all GA-generated files for this cycle under
//      calibration/ga_cycle_0001, ga_cycle_0002, ...
//
// Behavior:
//   keepDebugOutputs = false
//       - Maintain only ga_output.txt
//       - Maintain ga_output_merged.txt
//       - Do NOT create ga_cycle_xxxx folders
//
//   keepDebugOutputs = true
//       - Maintain ga_output.txt
//       - Maintain ga_output_merged.txt
//       - Create ga_cycle_xxxx folders and copy GA artifacts there
//
// Notes:
//   - Cycle 1 truncates the merged file so old runs do not accumulate.
//   - Cycle 2+ append to the merged file.
// ---------------------------------------------------------------------------
bool DTAssimilation::archiveGAOutput(int cycleIndex)
{
    const QString calibDir =
        QString::fromStdString(m_config.assimilation.calibrationOutputDir);

    QDir().mkpath(calibDir);

    const QString srcPath  = calibDir + "/ga_output.txt";
    const QString destPath = calibDir + "/ga_output_merged.txt";

    bool ok = false;

    // ---------------------------------------------------------------------
    // PART 1
    // Append current ga_output.txt to the cumulative merged file.
    //
    // First cycle:
    //     truncate existing merged file
    //
    // Later cycles:
    //     append
    // ---------------------------------------------------------------------
    QFile src(srcPath);

    if (src.exists() &&
        src.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QFile dest(destPath);

        const QIODevice::OpenMode mode =
            QIODevice::WriteOnly |
            QIODevice::Text |
            ((cycleIndex <= 1)
                 ? QIODevice::Truncate
                 : QIODevice::Append);

        if (dest.open(mode))
        {
            QTextStream out(&dest);

            const QString stamp =
                QDateTime::currentDateTimeUtc()
                    .toString("yyyy-MM-dd HH:mm:ss");

            const QString tNow =
                (m_buffer.pointCount() > 0)
                    ? QString::number(m_buffer.tMax(), 'f', 6)
                    : QStringLiteral("n/a");

            // Fresh merged file header
            if (cycleIndex <= 1)
            {
                out << "=====================================================\n";
                out << " OpenHydroTwin Assimilation GA Archive\n";
                out << " Started: " << stamp << "\n";
                out << "=====================================================\n\n";
            }

            // Cycle separator
            out << "=== Cycle " << cycleIndex
                << " | timestamp " << stamp
                << " | t_now=" << tNow
                << " ===\n";

            out << QString::fromUtf8(src.readAll());
            out << "\n";

            ok = true;
        }
        else
        {
            std::cerr
                << "[Assim] could not open merged GA file: "
                << destPath.toStdString()
                << "\n";
        }
    }
    else
    {
        std::cerr
            << "[Assim] GA output not found/readable: "
            << srcPath.toStdString()
            << "\n";
    }

    // ---------------------------------------------------------------------
    // Production mode:
    //
    // Do not keep per-cycle folders.
    // Return after maintaining ga_output_merged.txt.
    // ---------------------------------------------------------------------
    if (!m_config.keepDebugOutputs)
        return ok;

    // ---------------------------------------------------------------------
    // PART 2
    // Debug mode only.
    //
    // Preserve all current GA artifacts before the next calibration cycle
    // overwrites them.
    //
    // Example:
    //
    // calibration/
    //     ga_cycle_0001/
    //     ga_cycle_0002/
    //     ga_cycle_0003/
    // ---------------------------------------------------------------------
    const QString cycleDir =
        calibDir +
        QString("/ga_cycle_%1")
            .arg(cycleIndex, 4, 10, QChar('0'));

    QDir().mkpath(cycleDir);

    QDir dir(calibDir);

    const QFileInfoList files =
        dir.entryInfoList(QDir::Files |
                          QDir::NoDotAndDotDot);

    int copied = 0;

    for (const QFileInfo &fi : files)
    {
        const QString name = fi.fileName();

        // Skip files that are already long-term history products.
        if (name == "ga_output_merged.txt" ||
            name == "parameter_history.csv" ||
            name.startsWith("state_calibrated_"))
        {
            continue;
        }

        const QString dst = cycleDir + "/" + name;

        if (QFileInfo::exists(dst))
            QFile::remove(dst);

        if (QFile::copy(fi.absoluteFilePath(), dst))
            ++copied;
    }

    if (copied > 0)
    {
        std::cout
            << "[Assim] preserved "
            << copied
            << " GA artifact(s) in "
            << cycleDir.toStdString()
            << "\n";

        ok = true;
    }
    else
    {
        std::cerr
            << "[Assim] WARNING: no GA artifacts were copied from "
            << calibDir.toStdString()
            << "\n";
    }

    return ok;
}
// ---------------------------------------------------------------------------
// writeParameterLog
// Append a row to outputs/calibration/parameter_history.csv recording the
// best parameter values found in this calibration cycle. Header is written
// on the first cycle (when the file doesn't exist yet).
// ---------------------------------------------------------------------------
bool DTAssimilation::writeParameterLog(const System &sys, int cycleIndex)
{
    const QString calibDir =
        QString::fromStdString(m_config.assimilation.calibrationOutputDir);
    const QString filePath = calibDir + "/parameter_history.csv";

    const bool fileExists = QFileInfo::exists(filePath);

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        std::cerr << "[Assim] failed to open " << filePath.toStdString()
        << " for parameter log\n";
        return false;
    }

    QTextStream out(&file);
    const QString stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const double tMax = (m_buffer.pointCount() > 0) ? m_buffer.tMax() : 0.0;

    auto &params = const_cast<System &>(sys).Parameters();
    const int nParams = static_cast<int>(params.size());

    if (!fileExists)
    {
        out << "cycle,timestamp,t_now";
        for (int i = 0; i < nParams; ++i)
            out << "," << QString::fromStdString(params[i]->GetName());
        out << "\n";
    }

    out << cycleIndex << "," << stamp << "," << QString::number(tMax, 'f', 6);
    for (int i = 0; i < nParams; ++i)
    {
        const std::string val = params[i]->Variable("value")->GetProperty();
        out << "," << QString::fromStdString(val);
    }
    out << "\n";
    file.close();

    std::cout << "[Assim] parameter log updated: cycle " << cycleIndex
              << " → " << filePath.toStdString() << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// startTimer
// Starts the poll timer. Must be invoked on the assimilation thread; we rely
// on QThread::started → this slot via AutoConnection (becomes Queued across
// threads) to ensure the start() call happens on the correct thread.
// ---------------------------------------------------------------------------
void DTAssimilation::startTimer()
{
    if (m_started) return;

    if (m_pollIntervalWallClockMs <= 0)
    {
        std::cerr << "[Assim] startTimer() called before configure() — "
                  << "no interval set; timer will not start.\n";
        return;
    }

    // First refresh runs here, on the assim thread, so QNetworkAccessManager
    // (and any internal timers it owns) is constructed and used on the
    // thread it lives on. Doing this in configure() — which runs on the
    // main thread before moveToThread() — would trigger
    //   "QObject::startTimer: Timers cannot be started from another thread"
    // when the assim thread later tries to use the same NAM.
    if (!m_buffer.refresh())
    {
        std::cerr << "[Assim] initial refresh failed: "
                  << m_buffer.lastError().toStdString()
                  << " (will retry on poll timer)\n";
    }
    else
    {
        std::cout << "[Assim] initial refresh OK — "
                  << m_buffer.pointCount() << " points across "
                  << m_buffer.variableCount() << " variables\n";
        m_bufferTMax.store(m_buffer.tMax());
        m_bufferPointCount.store(static_cast<qint64>(m_buffer.pointCount()));
    }

    m_pollTimer.start(static_cast<int>(m_pollIntervalWallClockMs));
    m_started = true;

    std::cout << "[Assim] poll timer started on assim thread — "
              << m_config.assimilation.pollIntervalMs << " ms simulated, "
              << m_pollIntervalWallClockMs << " ms wall-clock"
              << " (acceleration " << m_config.timeAcceleration << "x)\n";
}
// ---------------------------------------------------------------------------
// stopTimer
// Stops the poll timer. Idempotent. Must be invoked on the assimilation
// thread (the timer lives there).
// ---------------------------------------------------------------------------
void DTAssimilation::stopTimer()
{
    if (m_pollTimer.isActive()) m_pollTimer.stop();
    m_started = false;
}

// ---------------------------------------------------------------------------
// recordCalibrationFailure
// One run_log row for a calibration cycle that failed before producing a
// valid simulation window or snapshot. Factored out of the (previously
// repeated) failure blocks in runCalibration.
// ---------------------------------------------------------------------------
void DTAssimilation::recordCalibrationFailure(const QDateTime &calStart,
                                              const QString  &errorMessage)
{
    if (!m_runLogger) return;
    m_runLogger->recordRun(
        RunLogger::RunType::AssimCalibration,
        m_cyclesCompleted + 1,
        calStart, QDateTime::currentDateTimeUtc(),
        -1.0, -1.0,                                // no valid sim window
        m_latestSnapshotPath,
        QString(),
        RunLogger::Status::Failed,
        errorMessage);
}

// ---------------------------------------------------------------------------
// runCalibration
// Thin dispatcher: shared, solver-agnostic preparation of the calibration
// System (prepareCalibrationSystem), then the inverse solve per the
// configured method — GA (deployed default) or MCMC (streaming Bayesian).
// ---------------------------------------------------------------------------
bool DTAssimilation::runCalibration(QString &errorMessage)
{
    const QDateTime calStart = QDateTime::currentDateTimeUtc();
    std::cout << "[Assim] [" << calStart.toString(Qt::ISODate).toStdString() << "] "
              << "calibration cycle " << (m_cyclesCompleted + 1) << " starting\n";

    System sys;
    double tStart = -1.0, tEnd = -1.0;
    if (!prepareCalibrationSystem(sys, tStart, tEnd, calStart, errorMessage))
        return false;

    if (m_config.assimilation.method == "MCMC")
        return runCalibrationMCMC(sys, tStart, tEnd, calStart, errorMessage);

    return runCalibrationGA(sys, tStart, tEnd, calStart, errorMessage);
}

// ---------------------------------------------------------------------------
// injectCalibrationWeather
// Fetch precipitation + the four Penman ET forcings over [tStart, tEnd] and
// inject them into `sys`. Shared by the calibration solve and the rolling-
// window spin-up. Best-effort: a failed fetch injects an empty series (the
// solver then sees no forcing for that gap) but never fails the cycle.
// ---------------------------------------------------------------------------
bool DTAssimilation::injectCalibrationWeather(System &sys,
                                              double tStart, double tEnd,
                                              QString &errorMessage)
{
    Q_UNUSED(errorMessage);
    const QDateTime windowStart = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>((tStart - 25569.0) * 86400000.0), Qt::UTC);
    const QDateTime windowEnd = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>((tEnd - 25569.0) * 86400000.0), Qt::UTC);

    std::cout << "[Assim] fetching weather for "
              << windowStart.toString(Qt::ISODate).toStdString() << " → "
              << windowEnd.toString(Qt::ISODate).toStdString() << "\n";

    CPrecipitation precip = DTWeather::fetchPrecipitation(
        m_config.weatherSource, m_config.latitude, m_config.longitude,
        windowStart, windowEnd);
    DTWeather::injectPrecipitation(&sys, precip);

    const std::string etSource = "Evapotranspiration_Penman (Soil)";

    const auto temp = DTWeather::fetchWeatherVariable(
        m_config.weatherSource, "temperature_2m",
        m_config.latitude, m_config.longitude, windowStart, windowEnd);
    DTWeather::injectWeather(&sys, etSource, "Temperature", temp);

    auto rh = DTWeather::fetchWeatherVariable(
        m_config.weatherSource, "relative_humidity_2m",
        m_config.latitude, m_config.longitude, windowStart, windowEnd);
    rh = rh / 100.0;
    DTWeather::injectWeather(&sys, etSource, "R_h", rh);

    const auto wind = DTWeather::fetchWeatherVariable(
        m_config.weatherSource, "windspeed_10m",
        m_config.latitude, m_config.longitude, windowStart, windowEnd);
    DTWeather::injectWeather(&sys, etSource, "wind_speed", wind);

    const auto rad = DTWeather::fetchWeatherVariable(
        m_config.weatherSource, "shortwave_radiation",
        m_config.latitude, m_config.longitude, windowStart, windowEnd);
    DTWeather::injectWeather(&sys, etSource, "solar_radiation", rad);

    return true;
}

// ---------------------------------------------------------------------------
// buildSpinupSnapshot
// One MAP-parameterized forward solve from t0 to tStart, starting from cold
// (script) initial conditions, whose captured end state becomes the fixed IC
// for every MCMC chain in a rolling-window cycle. Writes a state-capturing
// snapshot (SavetoJson calculatevalue=true) to spinPath.
// ---------------------------------------------------------------------------
bool DTAssimilation::buildSpinupSnapshot(double t0, double tStart,
                                         const QString &spinPath,
                                         QString &errorMessage)
{
    const std::string defaultTemplatePath =
        QCoreApplication::applicationDirPath().toStdString() + "/../../resources/";
    const std::string settingsFile = defaultTemplatePath + "settings.json";

    // 1. Read the latest MAP parameter values from the current snapshot.
    System mapSrc;
    mapSrc.SetDefaultTemplatePath(defaultTemplatePath);
    if (!mapSrc.ReadSystemSettingsTemplate(settingsFile) ||
        !mapSrc.LoadfromJson(m_latestSnapshotPath))
    {
        errorMessage = "spin-up: cannot load MAP snapshot " + m_latestSnapshotPath;
        return false;
    }
    const unsigned int np = mapSrc.ParametersCount();
    if (np == 0)
    {
        errorMessage = "spin-up: no parameters in MAP snapshot";
        return false;
    }
    std::vector<double> mapVals(np);
    for (unsigned int i = 0; i < np; ++i)
        mapVals[i] = mapSrc.Parameters()[i]->GetValue();

    // 2. Cold-start a fresh model from the script (state at t0 = script ICs).
    System spin;
    spin.SetDefaultTemplatePath(defaultTemplatePath);
    Script scr(m_config.scriptFile, &spin);
    spin.CreateFromScript(scr, settingsFile);
    if (spin.ParametersCount() != np)
    {
        errorMessage = QString("spin-up: script model has %1 parameters, "
                               "snapshot has %2").arg(spin.ParametersCount()).arg(np);
        return false;
    }

    // 3. Apply the MAP parameters.
    for (unsigned int i = 0; i < np; ++i)
        spin.SetParameterValue(static_cast<int>(i), mapVals[i]);
    spin.ApplyParameters();

    // 4. Window [t0, tStart] + weather forcing over it.
    spin.SetProp("simulation_start_time", t0);
    spin.SetProp("simulation_end_time",   tStart);
    spin.SetSystemSettings();
    spin.SetSilent(true);
    QString wErr;
    injectCalibrationWeather(spin, t0, tStart, wErr);

    // 5. Solve and capture the end state (calculatevalue=true) at tStart.
    if (!spin.Solve())
    {
        errorMessage = "spin-up: forward Solve failed over [t0, tStart]";
        return false;
    }
    if (!spin.SavetoJson(spinPath.toStdString(), {}, false, true))
    {
        errorMessage = "spin-up: cannot write IC snapshot " + spinPath;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// prepareCalibrationSystem
// Solver-agnostic calibration prep, shared by the GA and MCMC branches:
//   1. load the Settings template and the latest forward snapshot
//   2. pre-flight checks (parameters present, quantities verify)
//   3. push buffered observations into the selected Observations
//   4. patch the simulation window to the buffer span and inject weather
//      forcing covering it
// On success, sys is ready for an inverse solve over [tStart, tEnd].
// ---------------------------------------------------------------------------
bool DTAssimilation::prepareCalibrationSystem(System &sys,
                                              double &tStart,
                                              double &tEnd,
                                              const QDateTime &calStart,
                                              QString &errorMessage)
{

    auto mark = [](const QString &m) {
        DTDebugLog::instance().log(DTDebugLog::Category::Assim, m);
        DTDebugLog::instance().flush();
    };

    // Guard: need a snapshot to calibrate against.
    if (m_latestSnapshotPath.isEmpty())
    {
        errorMessage = "no forward snapshot available yet — calibration skipped";
        return false;
    }
    if (!QFileInfo::exists(m_latestSnapshotPath))
    {
        errorMessage = "snapshot path does not exist: " + m_latestSnapshotPath;
        return false;
    }

    // ---- Rolling calibration window ----
    // The scoring window is [tStart, tEnd] with tStart = tEnd - window. When
    // that start is later than the overall data start, a single MAP-
    // parameterized spin-up establishes the model state at tStart and the
    // calibration loads THAT snapshot as its base (so every chain shares the
    // same initial condition). Otherwise the window spans the whole record
    // and we load the latest forward snapshot directly (cold ICs at t0).
    if (m_buffer.pointCount() == 0)
    {
        errorMessage = "buffer is empty — nothing to calibrate against";
        return false;
    }
    const double tStartOverall = m_buffer.tMin();
    tEnd = m_buffer.tMax();
    const double windowDays = m_config.assimilation.calibrationWindowDays;
    tStart = (windowDays > 0.0)
                 ? std::max(tStartOverall, tEnd - windowDays)
                 : tStartOverall;

    QString baseSnapshotPath = m_latestSnapshotPath;
    if (tStart > tStartOverall + 1e-6)
    {
        const QString spinPath =
            QString::fromStdString(m_config.assimilation.calibrationOutputDir)
            + "/_spinup_ic.json";
        QString spinErr;
        if (buildSpinupSnapshot(tStartOverall, tStart, spinPath, spinErr))
        {
            baseSnapshotPath = spinPath;
            std::cout << "[Assim] rolling window: spun up IC at t=" << tStart
                      << " (scoring " << (tEnd - tStart) << " d of a "
                      << (tEnd - tStartOverall) << " d record)\n";
        }
        else
        {
            std::cerr << "[Assim] WARNING: spin-up failed (" << spinErr.toStdString()
                      << "); falling back to full window\n";
            tStart = tStartOverall;   // safe degrade — never breaks the cycle
        }
    }

    std::cout << "[Assim] loading snapshot: "
              << baseSnapshotPath.toStdString() << "\n";

    // 1. Load System from the base snapshot (spin-up IC when rolling, else the
    //    latest forward snapshot).
    //
    // Settings template must be loaded before LoadfromJson, otherwise the
    // Settings vector is empty and named Settings objects (Optimizer, MCMC,
    // Solver Settings, General Settings) won't be available via sys.object().
    // Mirrors what DTRunner does on the forward simulation path.
    const std::string defaultTemplatePath =
        QCoreApplication::applicationDirPath().toStdString() + "/../../resources/";
    const std::string settingsFile = defaultTemplatePath + "settings.json";
    sys.SetDefaultTemplatePath(defaultTemplatePath);
    if (!sys.ReadSystemSettingsTemplate(settingsFile))
    {
        errorMessage = "failed to load settings template from "
                       + QString::fromStdString(settingsFile);
        recordCalibrationFailure(calStart, errorMessage);
        return false;
    }

    std::cerr << "[Assim] settings template OK (" << sys.SettingsCount()
              << " objects)\n";

    try {
        if (!sys.LoadfromJson(baseSnapshotPath))
        {
            errorMessage = "System::LoadfromJson failed for " + baseSnapshotPath;
            recordCalibrationFailure(calStart, errorMessage);
            return false;
        }
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("LoadfromJson: ") + e.what());
    }


    // 2. Pre-flight checks (mirrors MainWindow::oninverserun).
    if (sys.ParametersCount() == 0)
    {
        errorMessage = "no Parameters defined in model — calibration skipped";
        recordCalibrationFailure(calStart, errorMessage);
        return false;
    }
    ErrorHandler errs = sys.VerifyAllQuantities();
    if (errs.Count() != 0)
    {
        std::cerr << "[Assim] verification errors (" << errs.Count() << "):\n";
        for (int i = 0; i < errs.Count(); ++i)
        {
            _error *e = errs[i];
            if (!e) continue;
            std::cerr << "  [" << i << "] "
                      << "object='" << e->objectname << "' "
                      << "class='"  << e->cls        << "' "
                      << "func='"   << e->funct      << "' "
                      << "code="    << e->code       << ": "
                      << e->description << "\n";
        }
        errorMessage = "model has verification errors";
        recordCalibrationFailure(calStart, errorMessage);
        return false;
    }

    // 3. Push buffered observations into the System Observations selected
    //    for calibration. If the config's calibration_observations list is
    //    empty, fall back to "use all matched" (backward-compatible).
    //    Observations not in the list have no observed_data set, so they
    //    contribute zero to the misfit but still get simulated and
    //    written to outputs for client-side visualization.
    const auto &selected = m_config.assimilation.calibrationObservations;
    const bool useAll = selected.empty();

    int matched = 0;
    int skipped = 0;
    for (unsigned int i = 0; i < sys.ObservationsCount(); ++i)
    {
        Observation *obs = sys.observation(i);
        const std::string name = obs->GetName();

        if (!useAll &&
            std::find(selected.begin(), selected.end(), name) == selected.end())
        {
            ++skipped;
            continue;
        }

        // Clamp observed data to the scoring window [tStart, tEnd]: with a
        // rolling window the model is only solved over that span, so points
        // before tStart have no modeled counterpart to compare against.
        TimeSeries<double> full = m_buffer.series(name);
        if (full.size() == 0) continue;
        TimeSeries<double> series;
        for (int p = 0; p < static_cast<int>(full.size()); ++p)
        {
            const double t = full.getTime(p);
            if (t >= tStart - 1e-9 && t <= tEnd + 1e-9)
                series.append(t, full.getValue(p));
        }
        if (series.size() == 0) continue;
        obs->Variable("observed_data")->SetTimeSeries(series);
        ++matched;
    }
    if (matched == 0)
    {
        errorMessage = "no buffered observations matched any selected "
                       "Observation by name";
        recordCalibrationFailure(calStart, errorMessage);
        return false;
    }
    std::cout << "[Assim] pushed observed_data into " << matched
              << " observation(s)";
    if (!useAll) std::cout << " (skipped " << skipped << " not in calibration list)";
    std::cout << "\n";

    // 4. Standard inverse-run prep (mirrors oninverserun).
    // The scoring window [tStart, tEnd] was computed above (rolling window;
    // possibly the full record). The temporal kernel down-weights older
    // observations WITHIN the window.

    // Read snapshot's window *before* we override it, for diagnostic
    double snapshotStart = -1.0, snapshotEnd = -1.0;
    if (Object *gs = sys.object("General Settings"))
    {
        try {
            snapshotStart = std::stod(gs->Variable("simulation_start_time")->GetProperty());
            snapshotEnd   = std::stod(gs->Variable("simulation_end_time")->GetProperty());
        } catch (...) {}
    }

    Object *generalSettings = sys.object("General Settings");
    if (!generalSettings)
    {
        errorMessage = "no 'General Settings' object found in System";
        recordCalibrationFailure(calStart, errorMessage);
        return false;
    }
    generalSettings->Variable("simulation_start_time")
        ->SetProperty(std::to_string(tStart));
    generalSettings->Variable("simulation_end_time")
        ->SetProperty(std::to_string(tEnd));

    // Belt-and-suspenders: also set on System directly. SetSystemSettings
    // will then propagate from the (now updated) Settings vector.
    sys.SetProp("simulation_start_time", tStart);
    sys.SetProp("simulation_end_time",   tEnd);
    sys.SetSystemSettings();
    sys.SetSilent(true);

    // Inject precip + Penman forcings over the scoring window (shared with
    // the spin-up via injectCalibrationWeather).
    {
        QString wErr;
        if (!injectCalibrationWeather(sys, tStart, tEnd, wErr))
        {
            errorMessage = wErr;
            recordCalibrationFailure(calStart, errorMessage);
            return false;
        }
    }

    std::cout << "[Assim] ===== Calibration window =====\n"
              << "[Assim]   buffer span:        " << tStart << " → " << tEnd
              << "  (" << (tEnd - tStart) << " days, "
              << m_buffer.pointCount() << " buffered points across "
              << m_buffer.variableCount() << " series)\n"
              << "[Assim]   snapshot was:       " << snapshotStart
              << " → " << snapshotEnd
              << "  (" << (snapshotEnd - snapshotStart) << " days)\n"
              << "[Assim]   solve window now:   " << tStart << " → " << tEnd
              << "  (" << (tEnd - tStart) << " days)\n";

    // Verify the override stuck after SetSystemSettings()
    if (Object *gs = sys.object("General Settings"))
    {
        try {
            const double finalStart = std::stod(gs->Variable("simulation_start_time")->GetProperty());
            const double finalEnd   = std::stod(gs->Variable("simulation_end_time")->GetProperty());
            std::cout << "[Assim]   verified post-set:  "
                      << finalStart << " → " << finalEnd << "\n";
            if (std::abs(finalStart - tStart) > 1e-6 ||
                std::abs(finalEnd   - tEnd  ) > 1e-6)
                std::cerr << "[Assim] WARNING: simulation window was overwritten\n";
        } catch (...) {}
    }
    std::cout << "[Assim] ==============================\n";

    return true;
}

// ---------------------------------------------------------------------------
// runCalibrationGA
// The deployed deterministic mode: GA inverse solve on the prepared System,
// reanalysis writeout, GA-output archival, calibrated snapshot, run-log row,
// and completion signal. Body unchanged from the pre-refactor runCalibration.
// ---------------------------------------------------------------------------
bool DTAssimilation::runCalibrationGA(System &sys,
                                      double tStart,
                                      double tEnd,
                                      const QDateTime &calStart,
                                      QString &errorMessage)
{
    // 5. GA setup.
    CGA<System> ga(&sys);
    Object *settings = sys.object("Optimizer");
    if (!settings)
    {
        errorMessage = "no 'Optimizer' object found in System "
                       "(this should not happen — Settings template loaded successfully)";
        return false;
    }
    ga.SetParameters(settings);

    sys.SetSystemSettings();

    std::cout << "[Assim] simulation window for GA: "
              << tStart << " → " << tEnd
              << " (" << (tEnd - tStart) << " days, "
              << m_buffer.pointCount() << " buffered points)\n";

    std::cout << "[Assim] GA configured from Settings 'Optimizer' ("
              << settings->GetVars()->size() << " quans)\n";

    const QString calibDir =
        QString::fromStdString(m_config.assimilation.calibrationOutputDir);
    QDir().mkpath(calibDir);
    ga.filenames.pathname       = calibDir.toStdString() + "/";
    ga.filenames.outputfilename = "ga_output.txt";

    // 6. Warm-start from previous cycle's terminal population, if available.
    const QString prevOutput = calibDir + "/ga_output.txt";
    if (m_cyclesCompleted > 0 && QFileInfo::exists(prevOutput))
    {
        ga.filenames.getfromfilename = prevOutput.toStdString();
        ga.getinifromoutput(prevOutput.toStdString());
        ga.getinitialpop(prevOutput.toStdString());
        std::cout << "[Assim] warm-starting from "
                  << prevOutput.toStdString() << "\n";
    }
    else
    {
        std::cout << "[Assim] cold-start GA (first calibration cycle)\n";
    }

    sys.SetParameterEstimationMode(parameter_estimation_options::inverse_model);

    // 7. Run.
    std::cout << "[Assim] running GA...\n";
    ga.optimize();

    sys.SetParameterEstimationMode();   // reset to default

    // 8. Transfer results onto sys (for snapshot writing).
    sys.TransferResultsFrom(&ga.Model_out);
    sys.Parameters() = ga.Model_out.Parameters();
    sys.SetOutputItems();

    ga.Model_out.Solve();

    // Make sure the deployment output directory exists before writing
    // reanalysis_output.csv. Without this, the write call can silently fail
    // on a fresh deployment or when outputDir was cleaned.
    const QString outputDir = QString::fromStdString(m_config.outputDir);
    QDir().mkpath(outputDir);

    const QString reanalysisPath = outputDir + "/reanalysis_output.csv";
    ga.Model_out.GetObservedOutputs().write(reanalysisPath.toStdString());

    if (QFileInfo::exists(reanalysisPath))
    {
        std::cout << "[Assim] reanalysis written: "
                  << reanalysisPath.toStdString() << "\n";
    }
    else
    {
        std::cerr << "[Assim] WARNING: reanalysis_output.csv was not created at "
                  << reanalysisPath.toStdString() << "\n";
    }

    // 9. Archive GA output to the merged file and preserve per-cycle GA
    // artifacts before the next cycle overwrites ga_output.txt.
    const int archiveCycle = m_cyclesCompleted + 1;
    if (!archiveGAOutput(archiveCycle))
    {
        std::cerr << "[Assim] failed to archive GA output for cycle "
                  << archiveCycle << "\n";
    }

    // 10. Write a new state snapshot reflecting the calibrated parameters.
    // In debug mode each cycle is timestamped and kept; otherwise we
    // overwrite a single fixed file so the calibration dir stays bounded
    // on long runs. DTRunner picks up whatever path is signaled.
    QString newSnapshotPath;
    if (m_config.keepDebugOutputs)
    {
        const QString stamp = QDateTime::currentDateTimeUtc()
        .toString("yyyyMMdd_HHmmss");
        newSnapshotPath = calibDir + "/state_calibrated_" + stamp + ".json";
    }
    else
    {
        newSnapshotPath = calibDir + "/state_calibrated_latest.json";
    }
    if (!sys.SavetoJson(newSnapshotPath.toStdString(), {}, false, false))
    {
        errorMessage = "failed to write calibrated snapshot: " + newSnapshotPath;
        recordCalibrationFailure(calStart, errorMessage);
        return false;
    }

    ++m_cyclesCompleted;
    std::cout << "[Assim] calibration cycle " << m_cyclesCompleted
              << " completed: " << newSnapshotPath.toStdString() << "\n";

    const QDateTime calEnd = QDateTime::currentDateTimeUtc();
    const qint64 elapsedMs = calStart.msecsTo(calEnd);
    std::cout << "[Assim] [" << calEnd.toString(Qt::ISODate).toStdString() << "] "
              << "calibration cycle " << m_cyclesCompleted
              << " finished in " << (elapsedMs / 1000.0) << " sec\n";

    writeParameterLog(sys, m_cyclesCompleted);

    if (m_runLogger)
    {
        std::cout << "[Assim] writing run_log row, runLogger ptr=" << m_runLogger << "\n";
        m_runLogger->recordRun(
            RunLogger::RunType::AssimCalibration,
            m_cyclesCompleted,
            calStart, calEnd,
            tStart, tEnd,
            m_latestSnapshotPath,
            newSnapshotPath,
            RunLogger::Status::Ok);
    }
    else
    {
        std::cout << "[Assim] runLogger is NULL — calibration not logged\n";
    }

    emit calibrationCompleted(newSnapshotPath);

    return true;
}

// ---------------------------------------------------------------------------
// runCalibrationMCMC
// Streaming Bayesian mode (spec Alg. 1). One calibration cycle:
//
//   deadline  <- cycle start + wall-clock budget (Sec. 3.8)
//   seed      <- posterior snapshot (cold / warm / provisional resume)
//   sample    <- runCycle(deadline)
//   publish   <- point estimate onto the calibrated System snapshot
//                (forward-loop adoption path identical to the GA's),
//                plus the posterior snapshot for the next cycle
//
// A cycle is NOT expected to converge (Sec. 3.9): a provisional outcome
// is a normal, successful cycle whose ensemble is carried forward, and is
// logged as Status::Ok with a "provisional" note — never as a failure.
// ---------------------------------------------------------------------------
bool DTAssimilation::runCalibrationMCMC(System &sys,
                                        double tStart,
                                        double tEnd,
                                        const QDateTime &calStart,
                                        QString &errorMessage)
{
    auto mark = [](const QString &m) {
        DTDebugLog::instance().log(DTDebugLog::Category::Assim, m);
        DTDebugLog::instance().flush();
    };

    // 5. Sampler setup.
    DTStreamingMCMC mcmc(&sys);

    Object *settings = sys.object("MCMC");
    if (!settings)
    {
        errorMessage = "no 'MCMC' object found in System "
                       "(this should not happen — Settings template loaded successfully)";
        recordCalibrationFailure(calStart, errorMessage);
        return false;
    }
    mcmc.SetParameters(settings);

    mcmc.streamSettings.realizationInterval = m_config.assimilation.mcmcRealizationInterval;
    mcmc.streamSettings.maxSweeps           = m_config.assimilation.mcmcMaxSweeps;

    std::cout << "[Assim] MCMC configured from Settings 'MCMC' ("
              << settings->GetVars()->size() << " quans), window "
              << tStart << " → " << tEnd
              << " (" << (tEnd - tStart) << " days, "
              << m_buffer.pointCount() << " buffered points)\n";

    const QString calibDir =
        QString::fromStdString(m_config.assimilation.calibrationOutputDir);
    QDir().mkpath(calibDir);

    // Per-evaluation detail logging only in debug mode; it serializes a
    // file append inside an omp critical on every forward solve.
    if (m_config.keepDebugOutputs)
    {
        mcmc.streamSettings.detailLogging = true;
        mcmc.FileInformation.detailfilename =
            calibDir.toStdString() + "/mcmc_details.txt";
    }

    // Wall-clock budget (Tcal, Sec. 3.8), anchored at CYCLE start: prep
    // time (snapshot load, weather fetch, observation push) counts against
    // the budget, so the whole cycle fits inside the calibration cadence
    // with only publication + one-sweep overshoot left for the margin. If
    // prep alone overran the budget, the deadline is already past and
    // runCycle degrades to a zero-sweep provisional cycle — loud in the
    // diagnostics, but never a failure.
    qint64 budgetMs = m_config.assimilation.mcmcBudgetMs;
    if (budgetMs <= 0)
        budgetMs = m_pollIntervalWallClockMs
                   - m_config.assimilation.mcmcBudgetMarginMs;
    if (budgetMs < 1000)
    {
        std::cerr << "[Assim] WARNING: derived MCMC budget is " << budgetMs
                  << " ms (cadence " << m_pollIntervalWallClockMs
                  << " ms minus margin "
                  << m_config.assimilation.mcmcBudgetMarginMs
                  << " ms); flooring at 1000 ms — check mcmc_budget / "
                     "mcmc_budget_margin in config.json\n";
        budgetMs = 1000;
    }
    const QDateTime deadline = calStart.addMSecs(budgetMs);
    std::cout << "[Assim] MCMC sampling deadline: "
              << deadline.toString(Qt::ISODate).toStdString()
              << " (budget " << budgetMs << " ms)\n";

    // 6. Cross-cycle state: previous pool / carried chain states /
    // proposal scales. A corrupt snapshot forfeits the warm start but
    // must not wedge the assimilation loop, so it degrades to cold start.
    const QString postPath =
        QString::fromStdString(m_config.assimilation.posteriorSnapshotPath);
    {
        QString loadErr;
        if (!mcmc.loadPosteriorSnapshot(postPath, loadErr))
        {
            std::cerr << "[Assim] WARNING: posterior snapshot unreadable ("
                      << loadErr.toStdString() << ") — cold-starting\n";
        }
    }

    const SeedMode mode = mcmc.chooseSeedMode(/*driftDetected=*/false);
    static const char *modeNames[] =
        { "cold start", "warm start", "ratio reseed", "provisional resume" };
    std::cout << "[Assim] MCMC seeding: "
              << modeNames[static_cast<int>(mode)] << "\n";

    sys.SetParameterEstimationMode(parameter_estimation_options::inverse_model);

    if (!mcmc.initializeCycle(mode, errorMessage))
    {
        sys.SetParameterEstimationMode();   // reset to default
        recordCalibrationFailure(calStart, errorMessage);
        return false;
    }

    // 7. Run (wall-clock-bounded; Alg. 1 lines 12-29).
    std::cout << "[Assim] running streaming MCMC...\n";
    const DTCycleResult result = mcmc.runCycle(deadline);

    sys.SetParameterEstimationMode();   // reset to default

    if (result.pointEstimate.empty())
    {
        errorMessage = "MCMC cycle produced no point estimate "
                       "(empty ensemble?)";
        recordCalibrationFailure(calStart, errorMessage);
        return false;
    }

    // 8. Apply the point estimate onto sys (for snapshot writing) —
    // sampled MAP from the pool when converged, best carried state when
    // provisional (Sec. 3.9). Mirrors the parameter application inside
    // CMCMC::posterior: SetParameterValue per index, then ApplyParameters.
    for (size_t i = 0; i < result.pointEstimate.size(); ++i)
        sys.SetParameterValue(static_cast<int>(i), result.pointEstimate[i]);
    sys.ApplyParameters();
    sys.SetOutputItems();

    // Reanalysis: solve a COPY under the point estimate so the calibrated
    // snapshot itself stays unsolved (same semantics as the GA branch,
    // which writes sys while solving ga.Model_out).
    const QString outputDir = QString::fromStdString(m_config.outputDir);
    QDir().mkpath(outputDir);
    const QString reanalysisPath = outputDir + "/reanalysis_output.csv";
    {
        System reanalysis = sys;
        reanalysis.SetSilent(true);
        reanalysis.Solve();
        reanalysis.GetObservedOutputs().write(reanalysisPath.toStdString());
    }
    if (QFileInfo::exists(reanalysisPath))
    {
        std::cout << "[Assim] reanalysis written: "
                  << reanalysisPath.toStdString() << "\n";
    }
    else
    {
        std::cerr << "[Assim] WARNING: reanalysis_output.csv was not created at "
                  << reanalysisPath.toStdString() << "\n";
    }

    // 9. Publish the posterior snapshot (full or provisional payload,
    // Sec. 3.9) — the next cycle's seed source. A write failure here
    // breaks cross-cycle continuity, so it fails the cycle.
    {
        QString writeErr;
        if (!mcmc.writePosteriorSnapshot(postPath, result, writeErr))
        {
            errorMessage = "failed to write posterior snapshot: " + writeErr;
            recordCalibrationFailure(calStart, errorMessage);
            return false;
        }
    }
    std::cout << "[Assim] posterior snapshot ("
              << (result.converged ? "full" : "provisional") << ") written: "
              << postPath.toStdString() << "\n";

    // Cumulative per-cycle history for the viewer (posterior_history.jsonl,
    // sibling of the GA's ga_output_merged.txt; ~1 KB/cycle, no samples).
    {
        const QString historyPath =
            calibDir + "/posterior_history.jsonl";
        QString histErr;
        if (!mcmc.appendHistoryRecord(historyPath, result, tEnd, histErr))
        {
            // Viewer convenience only — never fails the cycle.
            std::cerr << "[Assim] WARNING: " << histErr.toStdString() << "\n";
        }
    }

    // Parameter CI history: EVERY cycle (dense record, not interval-gated).
    {
        const QString ciPath = calibDir + "/parameter_ci_history.csv";
        QString ciErr;
        if (!mcmc.appendParameterCIRow(result, ciPath, tEnd, ciErr))
            std::cerr << "[Assim] WARNING: " << ciErr.toStdString() << "\n";
    }

    // Realization band + posterior distribution: on cycle 1 (early look)
    // and every Nth cycle thereafter.
    const int cyc      = static_cast<int>(m_cyclesCompleted + 1);
    const int interval = m_config.assimilation.mcmcRealizationInterval;
    const bool onSchedule = (cyc == 1) ||
                            (interval > 0 && cyc % interval == 0);
    DTDebugLog::instance().log(DTDebugLog::Category::Assim,
                               QString("realization gate: cyc=%1 converged=%2 interval=%3 onSchedule=%4")
                                   .arg(cyc).arg(result.converged).arg(interval).arg(onSchedule));
    DTDebugLog::instance().flush();

    // Publish the realization band + posterior distribution on schedule
    // (cycle 1 and every Nth cycle) regardless of certification. Both writers
    // emit provisional payloads from the retained pool/reservoir; a
    // non-converged cycle simply yields a wider, uncertified band rather than
    // no output. Data-availability (empty pool/reservoir) is handled inside
    // each writer.
    if (onSchedule)
    {
        QString rErr;
        if (!mcmc.produceRealizationCI(result, outputDir, tEnd, rErr))
        {
            std::cerr << "[Assim] WARNING: realizations: " << rErr.toStdString() << "\n";
            DTDebugLog::instance().log(DTDebugLog::Category::Assim,
                                       QString("realizations skipped: %1").arg(rErr));
            DTDebugLog::instance().flush();
        }
        QString dErr;
        if (!mcmc.writePosteriorDistribution(result, outputDir, tEnd, dErr))
        {
            std::cerr << "[Assim] WARNING: posterior dist: " << dErr.toStdString() << "\n";
            DTDebugLog::instance().log(DTDebugLog::Category::Assim,
                                       QString("posterior dist skipped: %1").arg(dErr));
            DTDebugLog::instance().flush();
        }
    }
    else
    {
        DTDebugLog::instance().log(DTDebugLog::Category::Assim,
                                   "realization block SKIPPED (gate false)");
        DTDebugLog::instance().flush();
    }

    // 10. Write the calibrated System snapshot — identical naming and
    // shape to the GA branch, so DTRunner's adoption path is unchanged.
    QString newSnapshotPath;
    if (m_config.keepDebugOutputs)
    {
        const QString stamp = QDateTime::currentDateTimeUtc()
        .toString("yyyyMMdd_HHmmss");
        newSnapshotPath = calibDir + "/state_calibrated_" + stamp + ".json";
    }
    else
    {
        newSnapshotPath = calibDir + "/state_calibrated_latest.json";
    }
    if (!sys.SavetoJson(newSnapshotPath.toStdString(), {}, false, false))
    {
        errorMessage = "failed to write calibrated snapshot: " + newSnapshotPath;
        recordCalibrationFailure(calStart, errorMessage);
        return false;
    }

    ++m_cyclesCompleted;

    const QDateTime calEnd = QDateTime::currentDateTimeUtc();
    const qint64 elapsedMs = calStart.msecsTo(calEnd);
    std::cout << "[Assim] [" << calEnd.toString(Qt::ISODate).toStdString() << "] "
              << "calibration cycle " << m_cyclesCompleted
              << " (MCMC, " << (result.converged ? "converged" : "provisional")
              << ", plateaued=" << result.plateauedFraction
              << ", pool=" << result.pooledSamples.size()
              << ") finished in " << (elapsedMs / 1000.0) << " sec: "
              << newSnapshotPath.toStdString() << "\n";

    writeParameterLog(sys, m_cyclesCompleted);

    // A provisional cycle IS a successful cycle: the ensemble moved
    // toward the target and its progress is banked in the posterior
    // snapshot. Status::Ok either way; the note column carries the
    // converged/provisional distinction for run-log inspection.
    if (m_runLogger)
    {
        m_runLogger->recordRun(
            RunLogger::RunType::AssimCalibration,
            m_cyclesCompleted,
            calStart, calEnd,
            tStart, tEnd,
            m_latestSnapshotPath,
            newSnapshotPath,
            RunLogger::Status::Ok,
            result.converged
                ? QString("MCMC converged (ess=%1)")
                      .arg(result.effectiveSampleSize)
                : QString("MCMC provisional (plateaued=%1)")
                      .arg(result.plateauedFraction));
    }

    emit calibrationCompleted(newSnapshotPath);

    return true;
}
