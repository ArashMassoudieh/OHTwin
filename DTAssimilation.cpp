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
    const bool ok = runCalibration(calErr);
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
// runCalibration
// Loads a System from the latest snapshot, pushes buffered observations into
// matching Observations, runs the GA inverse solver, and writes a calibrated
// snapshot. Mirrors the structure of MainWindow::oninverserun() in OHQ.
// ---------------------------------------------------------------------------
bool DTAssimilation::runCalibration(QString &errorMessage)
{
    const QDateTime calStart = QDateTime::currentDateTimeUtc();
    std::cout << "[Assim] [" << calStart.toString(Qt::ISODate).toStdString() << "] "
              << "calibration cycle " << (m_cyclesCompleted + 1) << " starting\n";

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

    std::cout << "[Assim] loading snapshot: "
              << m_latestSnapshotPath.toStdString() << "\n";

    // 1. Load System from the latest snapshot.
    System sys;

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
        errorMessage = "failed to load settings template from " + QString::fromStdString(settingsFile);
        if (m_runLogger)
        {
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
        return false;

    }

    if (!sys.LoadfromJson(m_latestSnapshotPath))
    {
        errorMessage = "System::LoadfromJson failed for " + m_latestSnapshotPath;
        if (m_runLogger)
        {
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
        return false;
    }

    // 2. Pre-flight checks (mirrors MainWindow::oninverserun).
    if (sys.ParametersCount() == 0)
    {
        errorMessage = "no Parameters defined in model — calibration skipped";
        if (m_runLogger)
        {
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

        if (m_runLogger)
        {
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
        return false;
    }

    // 3. Push buffered observations into the System Observations selected
    //    for calibration. If the config's calibration_observations list is
    //    empty, fall back to "use all matched" (backward-compatible).
    //    Observations not in the list have no observed_data set, so they
    //    contribute zero to the GA misfit but still get simulated and
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

        TimeSeries<double> series = m_buffer.series(name);
        if (series.size() == 0) continue;
        obs->Variable("observed_data")->SetTimeSeries(series);
        ++matched;
    }
    if (matched == 0)
    {
        errorMessage = "no buffered observations matched any selected "
                       "Observation by name";
        if (m_runLogger)
        {
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
        return false;
    }
    std::cout << "[Assim] pushed observed_data into " << matched
              << " observation(s)";
    if (!useAll) std::cout << " (skipped " << skipped << " not in calibration list)";
    std::cout << "\n";

    // 4. Standard inverse-run prep (mirrors oninverserun).
    // ===== ADD THIS BLOCK HERE =====
    // Adjust the simulation window to span all buffered observations,
    // so the GA evaluates each candidate over the full data window.
    // The temporal kernel handles down-weighting of older observations.
    if (m_buffer.pointCount() == 0)
    {
        errorMessage = "buffer is empty — nothing to calibrate against";
        return false;
    }
    const double tStart = m_buffer.tMin();
    const double tEnd   = m_buffer.tMax();

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
        if (m_runLogger)
        {
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
    // Inject precipitation covering the calibration window so the GA's
    // forward solves see the same forcing the truth twin saw.
    {
        // Convert OHQ day-serial to QDateTime.
        // ⚠ NEEDS YOUR EXISTING HELPER — see note below
        const QDateTime windowStart =
            QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>((tStart - 25569.0) * 86400000.0),
                Qt::UTC);
        const QDateTime windowEnd =
            QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>((tEnd - 25569.0) * 86400000.0),
                Qt::UTC);

        std::cout << "[Assim] fetching precipitation for "
                  << windowStart.toString(Qt::ISODate).toStdString()
                  << " → "
                  << windowEnd.toString(Qt::ISODate).toStdString() << "\n";

        CPrecipitation precip = DTWeather::fetchPrecipitation(
            m_config.weatherSource,
            m_config.latitude,
            m_config.longitude,
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

        rh = rh/100.0;
        DTWeather::injectWeather(&sys, etSource, "R_h", rh);

        const auto wind = DTWeather::fetchWeatherVariable(
            m_config.weatherSource, "windspeed_10m",
            m_config.latitude, m_config.longitude, windowStart, windowEnd);
        DTWeather::injectWeather(&sys, etSource, "wind_speed", wind);

        const auto rad = DTWeather::fetchWeatherVariable(
            m_config.weatherSource, "shortwave_radiation",
            m_config.latitude, m_config.longitude, windowStart, windowEnd);
        DTWeather::injectWeather(&sys, etSource, "solar_radiation", rad);
    }

    std::cout << "[Assim] ===== Calibration window =====\n"
              << "[Assim]   buffer span:        " << tStart << " → " << tEnd
              << "  (" << (tEnd - tStart) << " days, "
              << m_buffer.pointCount() << " buffered points across "
              << m_buffer.variableCount() << " series)\n"
              << "[Assim]   snapshot was:       " << snapshotStart
              << " → " << snapshotEnd
              << "  (" << (snapshotEnd - snapshotStart) << " days)\n"
              << "[Assim]   GA window now:      " << tStart << " → " << tEnd
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

    ga.Model_out.Solve();   // or whatever the forward path's solve call is

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
        if (m_runLogger)
        {
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
