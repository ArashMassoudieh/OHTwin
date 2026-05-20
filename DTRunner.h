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

#pragma once

#include "DTConfig.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <map>
#include <memory>
#include <string>
#include "noaaweatherfetcher.h"
#include "Precipitation.h"
#include "TimeSeriesSet.h"


class System;
class DTAssimilation;
class QThread;
class RunLogger;

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// StageKind
// Distinguishes the two solves performed in each cycle:
//   Advance  — short window [t, t+Δ]; its end-state drives the next cycle.
//   Forecast — longer window [t, t+Δ+H]; its outputs are published but the
//              end-state is discarded.
// ---------------------------------------------------------------------------
enum class StageKind { Advance, Forecast };

// ---------------------------------------------------------------------------
// StageResult
// What runStage() hands back to the orchestrator.  Holds the observed-output
// time series (for the selected_output.csv merge) plus a handful of paths
// useful for logging or downstream wiring.
// ---------------------------------------------------------------------------
struct StageResult
{
    bool                   ok = false;
    StageKind              kind = StageKind::Advance;
    TimeSeriesSet<double>  observed;       // copy of GetObservedOutputs()
    QString                outputFilePath; // e.g. *_output.txt or *_forecast_output.txt
    QString                vizSvgPath;     // viz.svg or forecast_viz.svg (Advance only writes state snapshot)
    QString                stateSnapshotPath; // populated for Advance stage only
};

// ---------------------------------------------------------------------------
// DTRunner
//
// Owns the simulation loop.  Each call to runOnce():
//   1. Determines the current simulation window [startTime, endTime] in OHQ
//      day-serial units.
//   2. Builds (first run) or hot-restarts (subsequent runs) the System.
//   3. Advances simulation_start_time / simulation_end_time in the loaded
//      state so the next interval picks up where this one left off.
//   4. Writes outputs to outputDir and a timestamped state snapshot to
//      stateDir.
//
// The runner is intentionally a QObject so it can be connected to QTimer
// and, later, to Crow API callbacks.
// ---------------------------------------------------------------------------
class DTRunner : public QObject
{
    Q_OBJECT

public:
    explicit DTRunner(const DTConfig &config, QObject *parent = nullptr);

    // Destructor is defined out-of-line in DTRunner.cpp so that the
    // unique_ptr<DTAssimilation> member can hold an incomplete type
    // here. (Forward declaration above keeps DTAssimilation.h out of
    // this header's transitive include set.)
    ~DTRunner() override;

    // Call once at start-up; initialises timing state.
    // Returns false if something is fatally wrong (bad config paths, etc.)
    bool init(QString &errorMessage);

public slots:
    // Owns the simulation loop.  Each call to runOnce():
    //   1. Runs the Advance stage on [t, t+Δ]; saves the end-state snapshot
    //      that drives the next cycle.
    //   2. Optionally runs the Forecast stage on [t, t+Δ+H] from the same
    //      initial state; its end-state is discarded.
    //   3. Merges both stages into selected_output.csv via read-knockout-append-
    //      write so the file always holds actuals up to t+Δ followed by the
    //      latest forecast tail.
    //
    // The runner is intentionally a QObject so it can be connected to QTimer
    // and, later, to Crow API callbacks.
    // ---------------------------------------------------------------------------
    bool runOnce();

    // Render-only utility: regenerate viz.svg and forecast_viz.svg without
    // running OHQ, fetching weather, advancing state, or modifying selected
    // outputs. If existing viz_state JSON files are present, they are used;
    // otherwise a static/default SVG is rendered directly from the deployment
    // viz_*.json layout.
    bool renderOnly();

    void onCalibrationCompleted(QString newSnapshotPath);

signals:
    // Emitted at the end of each successful runOnce() to hand off the
    // freshly-written state snapshot path to the assimilation thread.
    // Connected to DTAssimilation::setLatestSnapshot via AutoConnection,
    // which becomes QueuedConnection across threads.
    void snapshotReady(QString path);


private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    // Find the most-recent state snapshot in stateDir, if any.
    // Returns empty string if none found.
    QString findLatestStateSnapshot() const;

    // Build the timestamped snapshot filename:  state_YYYYMMDD_HHmmss.json
    QString makeSnapshotFilename(const QDateTime &dt) const;

    // Convert a QDateTime to an OHQ day-serial (days since 1899-12-30,
    // which is the Excel epoch used by OHQ).
    static double toOHQDaySerial(const QDateTime &dt);

    // Convert an OHQ day-serial back to QDateTime.
    static QDateTime fromOHQDaySerial(double serial);

    // Write a QJsonObject to a file; returns false on I/O error.
    static bool writeJson(const QJsonObject &obj, const QString &path);

    // Read a QJsonObject from a file; returns empty object on error.
    static QJsonObject readJson(const QString &path);

    // Patch simulation_start_time and simulation_end_time inside a state
    // JSON object and return the modified copy.
    static QJsonObject patchSimulationWindow(const QJsonObject &state,
                                             double startSerial,
                                             double endSerial);

    // Fetch NOAA precipitation and convert to CPrecipitation for the given window
    CPrecipitation fetchPrecipitation(const QDateTime &intervalStart,
                                      const QDateTime &intervalEnd);

    // Placeholder: inject precipitation into the OHQ system
    void injectPrecipitation(System *system, const CPrecipitation &precip);

    // -----------------------------------------------------------------------
    // Two-stage simulation helpers (Step 2 declarations)
    // -----------------------------------------------------------------------

    // Run a single solve over [stageStart, stageEnd].
    //   - For StageKind::Advance: saves model snapshot + state snapshot,
    //     writes viz, writes *_output.txt.
    //   - For StageKind::Forecast: writes *_forecast_output.txt and
    //     forecast_viz.svg only; no state side-effects.
    // Both stages start from the same initial-condition path
    // (modelJsonPath empty → cold start from script).
    StageResult runStage(StageKind kind,
                         const QDateTime &stageStart,
                         const QDateTime &stageEnd,
                         const QString &modelJsonPath);

    // Read selected_output.csv (if present), drop rows past `cutoffTime`,
    // append the Advance and Forecast observed series, write back.
    // `cutoffTime` is in OHQ day-serial units.
    bool mergeIntoSelectedOutput(const TimeSeriesSet<double> &advanceObs,
                                 const TimeSeriesSet<double> &forecastObs,
                                 double cutoffTime);

    // -----------------------------------------------------------------------
    // Synthetic-observation noise (Truth Twin)
    // -----------------------------------------------------------------------

    // Apply multiplicative log-normal noise driven by an Ornstein-Uhlenbeck
    // process with unit stationary variance:
    //     x_obs(t) = x_model(t) * exp(sigma * epsilon(t))
    // The OU state epsilon is *persisted across calls* in m_ouState
    // (one entry per series name) so correlation continues across cycles.
    // First time a series name is seen, epsilon is drawn from N(0,1)
    // (the stationary distribution).
    //
    // tauDays <= 0 is treated as the white-noise limit (each point gets
    // an independent N(0,1) draw).
    //
    // No-op if sigma <= 0.
    void applyOUNoiseStateful(TimeSeriesSet<double> &set,
                              double defaultSigma,
                              double tauDays);

    // Return the noise sigma for one observed-output series. The default
    // value comes from observations.noiseSigma; optional entries in
    // observations.noiseSigmaByPattern override it when their lower-case
    // key appears as a substring of the lower-case series name.
    double observationNoiseSigmaForSeries(const std::string &seriesName) const;

    // Write outputs/selected_output_meta.json describing the noise model
    // used to generate selected_output.csv (sigma, tau, save_interval, ts).
    // Idempotent: re-writes on every cycle so `last_updated` stays fresh.
    bool writeMetadataSidecar() const;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    const DTConfig &m_config;

    // Wall-clock datetime that corresponds to the *start* of the next
    // simulation interval.
    QDateTime m_nextIntervalStart;

    // OHQ interval length expressed in days (derived from config.intervalMs).
    double m_intervalDays = 1.0;

    // Forecast horizon in days (0 = forecast disabled, Stage B skipped).
    double m_forecastDays = 0.0;

    // How many intervals have completed successfully.
    int m_runsCompleted = 0;

    bool m_selectedOutputWritten = false;

    // -----------------------------------------------------------------------
    // Truth Twin / observation noise state
    // -----------------------------------------------------------------------

    // Save cadence for noisy observations, in OHQ days
    // (derived from m_config.observations.saveIntervalMs at init()).
    double m_obsSaveIntervalDays = 0.0;

    // Persistent OU state, one epsilon per observed-variable series name.
    // Updated in-place by applyOUNoiseStateful() so that correlation
    // continues smoothly across runOnce() cycles.
    std::map<std::string, double> m_ouState;

    // -----------------------------------------------------------------------
    // Data assimilation (forward twin only)
    // -----------------------------------------------------------------------
    // Owned only when m_config.assimilation.enabled is true. Pointer
    // (rather than value member) keeps the DTAssimilation header out of
    // DTRunner.h's transitive include set and avoids constructing the
    // QTimer-driven assimilation manager when assimilation is disabled.
    std::unique_ptr<DTAssimilation> m_assimilation;

    QString m_pendingCalibratedSnapshot;

    // Dedicated thread the assimilation object lives on after
    // moveToThread().  Parented to *this* (QObject ownership), but
    // ~DTRunner() explicitly calls quit()/wait() on it before
    // m_assimilation is destroyed, to guarantee no timer tick fires
    // onto a half-destroyed DTAssimilation.  nullptr when assimilation
    // is disabled.
    QThread *m_assimThread = nullptr;

    // Owned for the entire lifetime of the runner; shared with
    // m_assimilation (raw pointer) so calibration cycles can append rows.
    std::unique_ptr<RunLogger> m_runLogger;


};

QDateTime fromOHQDaySerial(double serial);


