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

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// StateVarExport
// A variable name and the path where its JSON snapshot should be written.
// ---------------------------------------------------------------------------
struct StateVarExport
{
    std::string variable;
    std::string outputPath;
};

// ---------------------------------------------------------------------------
// ObservationConfig
// Settings controlling synthetic-observation generation by the Truth Twin.
//
//   saveIntervalMs        : cadence at which observation snapshots are written
//                           (independent of the model integration step;
//                           defaults to the runtime interval if omitted).
//   noiseSigma            : default standard deviation of the multiplicative
//                           log-normal noise applied to observed values:
//                               x_obs = x_model * exp(sigma * epsilon)
//                           where epsilon is an OU process with unit
//                           stationary variance. 0 = noise disabled unless
//                           overridden per series in noiseSigmaByPattern.
//   noiseSigmaByPattern   : optional per-observation sigma overrides. Keys
//                           are lower-case substring patterns matched against
//                           each observed-output series name. For example,
//                           "soil moisture": 0.05 or "ert": 0.05.
//                           The JSON key "default" maps to noiseSigma.
//   noiseCorrelationTimeMs: correlation time tau of the OU process.
//                           0 = white-noise limit.
// ---------------------------------------------------------------------------
struct ObservationConfig
{
    qint64 saveIntervalMs         = 0;   // 0 sentinel -> fall back to runtime intervalMs
    double noiseSigma             = 0.0; // default / backward-compatible scalar
    std::map<std::string, double> noiseSigmaByPattern;
    qint64 noiseCorrelationTimeMs = 0;
};

// ---------------------------------------------------------------------------
// ParameterDriftEntry
// One time-varying parameter for the Truth Twin.
//
//   parameter : name of the OHQ parameter to update each cycle, as accepted
//               by System::GetParameter(name).
//   csvPath   : absolute path to a CSV readable by TimeSeries<double>. The
//               time axis is in OHQ day-serial units (Excel epoch); values
//               are interpolated to the current simulation time before each
//               Advance stage and applied via Parameter::SetValue().
//
// The block is OPTIONAL in config.json. If absent or empty, no drift is
// applied and the Truth Twin behaves exactly as a forward simulator.
// ---------------------------------------------------------------------------
struct ParameterDriftEntry
{
    std::string parameter;
    std::string csvPath;
};

// ---------------------------------------------------------------------------
// AssimilationConfig
// Settings for pulling observations from a Truth Twin (or, eventually,
// a real sensor stream) and for running data assimilation against them.
//
// The block is OPTIONAL in config.json. If absent, the runner does not
// poll any observation source and assimilation is fully disabled — the
// twin behaves as a pure forward simulator.
//
//   truthCsvUrl    : URL of the observation CSV (e.g. selected_output.csv
//                    served over HTTP by the Truth Twin's nginx).
//   truthMetaUrl   : URL of the metadata sidecar JSON describing the noise
//                    model used to generate the observations. Optional;
//                    if empty, the buffer falls back to sigma = 0.
//   pollIntervalMs : how often the assimilation manager polls the source.
//                    Independent of the forward-simulation interval.
// ---------------------------------------------------------------------------
struct AssimilationConfig
{
    bool        enabled        = false;        // true iff "assimilation" block present
    std::string truthCsvUrl;
    std::string truthMetaUrl;
    qint64      pollIntervalMs = 0;
    std::string calibrationOutputDir;          // resolved absolute path to calibration outputs
    // Names of observations to use in GA misfit. Empty => use all
    // observations matched in the buffer (backward-compatible).
    std::vector<std::string> calibrationObservations;

    // --- inverse solver selection ---
    // "GA"   : deterministic genetic-algorithm mode (deployed default)
    // "MCMC" : streaming Bayesian mode (DTStreamingMCMC)
    std::string method = "GA";

    // --- MCMC wall-clock budget (Tcal, spec Sec. 3.8) ---
    // Per-cycle sampling deadline = cycle start + budget. If
    // mcmcBudgetMs > 0 it is used directly; otherwise the budget is
    // derived as (wall-clock calibration cadence - mcmcBudgetMarginMs),
    // where the margin absorbs prep (snapshot load, weather fetch,
    // observation push), publication, and up to one forward-solve of
    // deadline overshoot. Ignored when method == "GA".
    qint64 mcmcBudgetMs       = 0;
    qint64 mcmcBudgetMarginMs = 30000;

    // Path of the posterior snapshot exchanged between cycles. Empty =>
    // calibrationOutputDir + "/posterior_latest.json".
    std::string posteriorSnapshotPath;
};

// ---------------------------------------------------------------------------
// DTConfig
//
// Loads and validates a deployment's config.json.
//
// Each deployment is a self-contained directory:
//
//   <deployment_root>/
//     config.json           (Option-A: { deployment{...}, runtime{...} })
//     viz_<name>.json       (path is given by deployment.viz_file)
//     model/<name>.ohq      (path is given by deployment.model_file)
//     state/                (auto-created: state snapshots)
//     outputs/              (auto-created: simulation outputs)
//     snapshots/            (auto-created: per-interval model JSON snapshots)
//
// Relative paths inside config.json (model_file, viz_file, output_path) are
// resolved against deployment_root. The auto-derived working directories
// (state/, outputs/, snapshots/) are created if missing.
//
// All final paths are stored as absolute std::string for direct OHQ use.
// ---------------------------------------------------------------------------
class DTConfig
{
public:
    // Load <deploymentRoot>/config.json.
    // Returns true on success; populates errorMessage on failure.
    bool load(const QString &deploymentRoot, QString &errorMessage);

    // --- deployment metadata ---
    std::string deploymentRoot;    // absolute path to the deployment directory
    std::string deploymentName;    // canonical name from config (deployment.name)
    int         port = 0;          // nginx/HTTP port for this deployment

    // --- paths (all absolute after load()) ---
    std::string scriptFile;        // .ohq script used on cold start
    std::string vizFile;           // visualization JSON spec for VizRenderer
    std::string stateDir;          // <deployment_root>/state
    std::string outputDir;         // <deployment_root>/outputs
    std::string modelSnapshotDir;  // <deployment_root>/snapshots
    std::string weatherFile;       // optional weather JSON (absolute path)
    std::string loadModelJson;     // optional cold-start model JSON (absolute path)

    // --- weather / Open-Meteo ---
    std::string weatherSource = "openmeteo";
    double      latitude      = 0.0;
    double      longitude     = 0.0;

    // --- timing ---
    // Accepted syntax: "300s", "15min", "4hr", "1day".
    std::string intervalStr;
    qint64      intervalMs = 86400000; // default: 1 day

    // Optional fixed start datetime (ISO 8601). Empty = use current time / snapshot.
    std::string startDatetime;

    // Optional stop datetime (ISO 8601). When the next interval start reaches
    // this point, the runner triggers QCoreApplication::quit().
    // Empty = run forever (real-time / continuously-updated forecast mode).
    std::string stopDatetime;

    // Wall-clock acceleration factor. The QTimer fires every
    // (intervalMs / timeAcceleration) milliseconds, but each tick still
    // advances `intervalMs` of *simulated* time.
    //   1.0  = real time (default)
    //   100  = 100x faster than real time
    //   < 0  = invalid (rejected at load)
    // Useful for historical / Truth-Twin runs.
    double      timeAcceleration = 1.0;

    // When true (and assimilation is enabled), runOnce advances simulated
    // time from m_nextIntervalStart all the way to the most recent
    // observation timestamp in the buffer (m_assimilation->buffer().tMax())
    // in a single forward step. This lets the assim twin catch up after
    // calibration cycles that would otherwise have left it behind.
    // Default false preserves the existing fixed-interval behavior.
    bool        advanceToObservations = false;

    // --- debug output retention ---
    // When false (default), per-cycle diagnostic files are suppressed:
    //
    //   outputs/<stamp>_output.txt
    //   outputs/<stamp>_forecast_output.txt
    //   outputs/<stamp>_advance_precipitation.txt
    //   outputs/<stamp>_forecast_precipitation.txt
    //   snapshots/<stamp>_model.json   (replaced by snapshots/_latest_model.json)
    //   <calibration_output_dir>/state_calibrated_<stamp>.json
    //                                  (replaced by state_calibrated_latest.json)
    //
    // Single-file outputs needed by the viewer (viz.svg, viz_state.json,
    // forecast_viz.svg, selected_output.csv, selected_output_meta.json) and
    // accumulating logs needed for analysis (ga_output.txt,
    // ga_output_merged.txt, parameter_history.csv) are always written.
    //
    // On a server running indefinitely, leave this off to keep disk usage
    // bounded. Turn on temporarily for development / debugging when a full
    // per-cycle audit trail is needed.
    bool        keepDebugOutputs = false;

    // --- forecast horizon ---
    // Optional duration of Stage B forecast window beyond the advance interval.
    // Same syntax as intervalStr. Empty / 0 = forecast disabled.
    std::string forecastHorizonStr;
    qint64      forecastHorizonMs = 0;

    // --- state variable exports ---
    std::vector<StateVarExport> stateVarExports;

    // --- synthetic observations (Truth Twin) ---
    // Controls the cadence and noise model for observation files written by
    // the Truth Twin. All fields are optional; if the "observations" block
    // is omitted from config.json, defaults yield no noise and saving at
    // the runtime interval (i.e., behaves identically to a forward twin).
    ObservationConfig observations;

    // --- data assimilation (forward twin) ---
    // Controls observation polling and (eventually) calibration. The whole
    // block is optional; if absent, .enabled is false and assimilation is
    // not initialized at all.
    AssimilationConfig assimilation;


    static bool eraseDirectoryContents(const QString &dirPath, QString &err);

    // --- parameter drift (Truth Twin) ---
    // Time-varying parameter overrides driven by external CSV time series.
    // Empty vector = no drift (default).
    std::vector<ParameterDriftEntry> parameterDrift;

private:
    // Parse "300s", "15min", "4hr", "1day" -> milliseconds.
    // Returns -1 on parse error and writes details into err.
    static qint64 parseIntervalMs(const std::string &s, QString &err);

    // Resolve a path string against deploymentRoot. Absolute paths are
    // returned as-is; relative paths are joined with deploymentRoot.
    // Empty input returns empty.
    QString resolvePath(const QString &p) const;
};
