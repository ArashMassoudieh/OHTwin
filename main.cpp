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

/*
 * OpenHydroQual Digital Twin — generic OHQ model runner
 *
 * Each invocation targets one self-contained deployment directory:
 *
 *   OHTwin --deployment /path/to/deployments/Bioretention
 *
 * The deployment directory holds config.json, the .ohq model file, the
 * visualization spec, and the runtime state/outputs/snapshots folders.
 *
 * main.cpp is intentionally thin:
 *   - Parse command-line arguments
 *   - Load config from the deployment directory
 *   - Initialise runner
 *   - Fire an immediate first run, then arm QTimer for subsequent intervals
 *   - (Future) Start Crow HTTP API
 */

#include "DTConfig.h"
#include "DTRunner.h"
#include "DTDebugLog.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

#include <algorithm>
#include <climits>
#include <iostream>


int main(int argc, char *argv[])
{
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
        fprintf(stderr, "[Qt] %s\n", msg.toLocal8Bit().constData());
        if (msg.contains("Timers cannot be started")) {
            std::abort();
        }
    });

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("OHTwin");
    QCoreApplication::setApplicationVersion("1.0");

    // ------------------------------------------------------------------
    // 1. Parse command-line arguments
    // ------------------------------------------------------------------
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "OpenHydroQual Digital Twin — runs one deployment");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption deploymentOpt(
        QStringList() << "d" << "deployment",
        "Path to the deployment directory containing config.json.",
        "path");
    parser.addOption(deploymentOpt);

    QCommandLineOption freshOpt(
        QStringList() << "fresh",
        "Erase contents of state/ and outputs/ before starting. Prompts for "
        "confirmation unless --force is also passed.");
    parser.addOption(freshOpt);

    QCommandLineOption forceOpt(
        QStringList() << "force",
        "Skip the confirmation prompt for --fresh.");
    parser.addOption(forceOpt);

    QCommandLineOption renderOnlyOpt(
        QStringList() << "render-only",
        "Skip simulation and regenerate viz.svg / forecast_viz.svg from existing viz_state JSON files, or static SVGs from the deployment viz spec if state JSON is missing.");
    parser.addOption(renderOnlyOpt);

    parser.process(app);

    if (!parser.isSet(deploymentOpt))
    {
        std::cerr << "[Main] --deployment <path> is required.\n"
                  << "       Example: OHTwin --deployment "
                     "/home/arash/Projects/DrywellDT/deployments/Bioretention\n";
        return 1;
    }

    const QString deploymentRoot = parser.value(deploymentOpt);

    // ------------------------------------------------------------------
    // 2. Load the deployment's config.json
    // ------------------------------------------------------------------
    DTConfig config;
    QString configError;
    if (!config.load(deploymentRoot, configError))
    {
        std::cerr << "[Main] Failed to load config: "
                  << configError.toStdString() << "\n";
        return 1;
    }

    {
        QString logErr;
        if (!DTDebugLog::instance().configure(
                QString::fromStdString(config.logging.filePath),
                config.logging.enabled,
                config.logging.categories,
                config.logging.flushEvery,
                config.logging.truncate,
                logErr))
        {
            std::cerr << "[Main] debug log setup failed: "
                      << logErr.toStdString() << "\n";
            return 1;
        }
    }

    // ------------------------------------------------------------------
    // 2a. Handle --fresh: erase contents of state/ and outputs/
    // ------------------------------------------------------------------
    // ------------------------------------------------------------------
    // 2a. Handle --fresh: erase contents of state/ and outputs/
    // ------------------------------------------------------------------
    if (parser.isSet(freshOpt))
    {
        QString freshErr;
        if (!DTConfig::eraseDirectoryContents(
                QString::fromStdString(config.stateDir), freshErr))
        {
            std::cerr << "[Fresh] state/: " << freshErr.toStdString() << "\n";
            return 1;
        }
        if (!DTConfig::eraseDirectoryContents(
                QString::fromStdString(config.outputDir), freshErr))
        {
            std::cerr << "[Fresh] outputs/: " << freshErr.toStdString() << "\n";
            return 1;
        }

        std::cout << "[Fresh] Erased contents of:\n"
                  << "  " << config.stateDir  << "\n"
                  << "  " << config.outputDir << "\n";
    }
    // ------------------------------------------------------------------
    // 3. Construct runner. In render-only mode, do not initialise the
    //    simulation loop, assimilation, timing state, or any OHQ state.
    //    Rendering only needs the deployment-resolved viz file and the
    //    existing outputs/*viz_state.json files when present; otherwise it falls back to the static deployment viz layout.
    // ------------------------------------------------------------------
    DTRunner runner(config);

    if (parser.isSet(renderOnlyOpt))
    {
        const bool ok = runner.renderOnly();
        return ok ? 0 : 4;
    }

    QString initError;
    if (!runner.init(initError))
    {
        std::cerr << "[Main] Runner init failed: "
                  << initError.toStdString() << "\n";
        return 2;
    }

    // ------------------------------------------------------------------
    // 4. Run the first interval immediately at start-up
    // ------------------------------------------------------------------
    // Use a single-shot zero-delay timer so the event loop is running before
    // the first solve begins. This keeps the door open for signals/slots and
    // the future Crow API to be wired in before any blocking work.
    QTimer::singleShot(0, &runner, [&runner]() {
        if (!runner.runOnce())
        {
            std::cerr << "[Main] Initial run failed.\n";
            QCoreApplication::exit(3);
        }
    });

    // ------------------------------------------------------------------
    // 5. Arm the recurring timer for subsequent intervals
    // ------------------------------------------------------------------
    // Wall-clock interval = simulated_interval / time_acceleration.
    // At time_acceleration=1.0 this is plain real-time. At higher values,
    // wall-clock ticks fire faster while the simulation still advances
    // intervalMs of *simulated* time per tick — used by Truth-Twin /
    // historical replay deployments.
    QTimer intervalTimer;

    const double wallClockIntervalMsD =
        static_cast<double>(config.intervalMs) / config.timeAcceleration;
    const qint64 wallClockIntervalMs =
        static_cast<qint64>(std::max(1.0, wallClockIntervalMsD));

    const qint64 safeIntervalMs =
        std::min(wallClockIntervalMs, static_cast<qint64>(INT_MAX));

    intervalTimer.setInterval(static_cast<int>(safeIntervalMs));

    QObject::connect(&intervalTimer, &QTimer::timeout,
                     &runner, [&runner]() {
                         if (!runner.runOnce())
                         {
                             std::cerr << "[Main] Periodic run failed. "
                                          "Continuing to next interval.\n";
                             // Non-fatal: log and wait for the next tick.
                         }
                     });

    intervalTimer.start();

    std::cout << "[Main] OHQ Digital Twin running. Deployment: "
              << config.deploymentName
              << "  Sim interval: " << config.intervalStr
              << "  (" << config.intervalMs << " ms)"
              << "  Wall-clock tick: " << safeIntervalMs << " ms"
              << "  Acceleration: " << config.timeAcceleration << "x\n"
              << "[Main] Press Ctrl+C to stop.\n";

    // ------------------------------------------------------------------
    // 6. TODO: wire in Crow HTTP API here before app.exec()
    // ------------------------------------------------------------------

    return app.exec();
}
