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

#include "DTConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cctype>
#include <iostream>
#include <algorithm>

// ---------------------------------------------------------------------------
// DTConfig::parseIntervalMs
// ---------------------------------------------------------------------------
qint64 DTConfig::parseIntervalMs(const std::string &s, QString &err)
{
    const QString raw = QString::fromStdString(s).trimmed();
    const std::string trimmed = raw.toStdString();

    if (trimmed.empty())
    {
        err = "interval string is empty";
        return -1;
    }

    size_t i = 0;
    while (i < trimmed.size() &&
           (std::isdigit(static_cast<unsigned char>(trimmed[i])) || trimmed[i] == '.'))
        ++i;

    if (i == 0)
    {
        err = QString("interval '%1' has no leading number").arg(raw);
        return -1;
    }

    double value = 0.0;
    try
    {
        value = std::stod(trimmed.substr(0, i));
    }
    catch (...)
    {
        err = QString("invalid numeric value in interval '%1'").arg(raw);
        return -1;
    }

    const std::string unit = trimmed.substr(i);

    qint64 multiplierMs = 0;
    if      (unit == "s")   multiplierMs = 1000LL;
    else if (unit == "min") multiplierMs = 60LL   * 1000LL;
    else if (unit == "hr")  multiplierMs = 3600LL * 1000LL;
    else if (unit == "day") multiplierMs = 86400LL * 1000LL;
    else
    {
        err = QString("unknown interval unit '%1' (use s, min, hr, day)")
        .arg(QString::fromStdString(unit));
        return -1;
    }

    const qint64 result = static_cast<qint64>(value * static_cast<double>(multiplierMs));
    if (result <= 0)
    {
        err = QString("interval must be > 0, got '%1'").arg(raw);
        return -1;
    }

    return result;
}

// ---------------------------------------------------------------------------
// DTConfig::resolvePath
// ---------------------------------------------------------------------------
QString DTConfig::resolvePath(const QString &p) const
{
    if (p.isEmpty()) return {};
    if (QDir::isAbsolutePath(p)) return QDir::cleanPath(p);
    return QDir::cleanPath(QString::fromStdString(deploymentRoot) + "/" + p);
}

// ---------------------------------------------------------------------------
// DTConfig::load
// ---------------------------------------------------------------------------
bool DTConfig::load(const QString &deploymentRootIn, QString &errorMessage)
{
    // ------------------------------------------------------------------
    // Resolve deployment root
    // ------------------------------------------------------------------
    const QFileInfo rootInfo(deploymentRootIn);
    if (!rootInfo.exists() || !rootInfo.isDir())
    {
        errorMessage = "deployment path is not a directory: " + deploymentRootIn;
        return false;
    }
    deploymentRoot = rootInfo.absoluteFilePath().toStdString();

    const QString configPath =
        QDir(QString::fromStdString(deploymentRoot)).absoluteFilePath("config.json");

    QFile file(configPath);
    if (!file.exists())
    {
        errorMessage = "config.json not found at: " + configPath;
        return false;
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        errorMessage = "Cannot open config.json: " + configPath;
        return false;
    }

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (doc.isNull())
    {
        errorMessage = "config.json parse error: " + parseErr.errorString();
        return false;
    }
    if (!doc.isObject())
    {
        errorMessage = "config.json root must be a JSON object";
        return false;
    }

    const QJsonObject root = doc.object();
    stateVarExports.clear();

    // ------------------------------------------------------------------
    // deployment{}
    // ------------------------------------------------------------------
    if (!root.contains("deployment") || !root.value("deployment").isObject())
    {
        errorMessage = "config.json is missing required 'deployment' object";
        return false;
    }
    const QJsonObject dep = root.value("deployment").toObject();

    deploymentName = dep.value("name").toString().trimmed().toStdString();
    if (deploymentName.empty())
    {
        // Fall back to the directory name if the config omits it.
        deploymentName = rootInfo.fileName().toStdString();
    }

    port = dep.value("port").toInt(0);
    if (port <= 0)
    {
        errorMessage = "config.json deployment.port must be a positive integer";
        return false;
    }

    const QString modelFileQ = dep.value("model_file").toString().trimmed();
    if (modelFileQ.isEmpty())
    {
        errorMessage = "config.json deployment.model_file is required";
        return false;
    }
    scriptFile = resolvePath(modelFileQ).toStdString();

    const QString vizFileQ = dep.value("viz_file").toString().trimmed();
    if (vizFileQ.isEmpty())
    {
        errorMessage = "config.json deployment.viz_file is required";
        return false;
    }
    vizFile = resolvePath(vizFileQ).toStdString();

    // ------------------------------------------------------------------
    // runtime{}
    // ------------------------------------------------------------------
    if (!root.contains("runtime") || !root.value("runtime").isObject())
    {
        errorMessage = "config.json is missing required 'runtime' object";
        return false;
    }
    const QJsonObject rt = root.value("runtime").toObject();

    weatherSource = rt.value("weather_source").toString("openmeteo").trimmed().toStdString();
    latitude      = rt.value("latitude").toDouble(0.0);
    longitude     = rt.value("longitude").toDouble(0.0);

    startDatetime      = rt.value("start_datetime").toString().trimmed().toStdString();
    stopDatetime       = rt.value("stop_datetime").toString().trimmed().toStdString();
    intervalStr        = rt.value("interval").toString("1day").trimmed().toStdString();
    forecastHorizonStr = rt.value("forecast_horizon").toString().trimmed().toStdString();

    timeAcceleration = rt.value("time_acceleration").toDouble(1.0);
    if (timeAcceleration <= 0.0)
    {
        errorMessage = "config.json runtime.time_acceleration must be > 0 "
                       "(got " + QString::number(timeAcceleration) + ")";
        return false;
    }

    advanceToObservations = rt.value("advance_to_observations").toBool(false);
    keepDebugOutputs      = rt.value("keep_debug_outputs").toBool(false);

    QString intervalErr;
    intervalMs = parseIntervalMs(intervalStr, intervalErr);
    if (intervalMs < 0)
    {
        errorMessage = "config.json runtime.interval error: " + intervalErr;
        return false;
    }

    if (!forecastHorizonStr.empty())
    {
        QString horizonErr;
        forecastHorizonMs = parseIntervalMs(forecastHorizonStr, horizonErr);
        if (forecastHorizonMs < 0)
        {
            errorMessage = "config.json runtime.forecast_horizon error: " + horizonErr;
            return false;
        }
    }
    else
    {
        forecastHorizonMs = 0;
    }

    // Optional cold-start / weather paths (relative to deployment root).
    loadModelJson =
        resolvePath(rt.value("load_model_json").toString().trimmed()).toStdString();
    weatherFile =
        resolvePath(rt.value("weather_file").toString().trimmed()).toStdString();

    // ------------------------------------------------------------------
    // state_variables (relative output_paths -> deployment root)
    // ------------------------------------------------------------------
    const QJsonArray stateVars = rt.value("state_variables").toArray();
    for (const auto &entry : stateVars)
    {
        if (!entry.isObject()) continue;

        const QJsonObject obj = entry.toObject();
        StateVarExport exp;
        exp.variable   = obj.value("variable").toString().toStdString();
        exp.outputPath = resolvePath(obj.value("output_path").toString()).toStdString();

        if (!exp.variable.empty() && !exp.outputPath.empty())
            stateVarExports.push_back(exp);
    }

    // ------------------------------------------------------------------
    // observations{} (optional; controls Truth Twin noise & save cadence)
    // ------------------------------------------------------------------
    // Defaults: no noise (sigma=0, tau=0) and save at the runtime interval.
    observations.saveIntervalMs         = intervalMs;  // fallback: model interval
    observations.noiseSigma             = 0.0;
    observations.noiseSigmaByPattern.clear();
    observations.noiseCorrelationTimeMs = 0;

    if (root.contains("observations"))
    {
        if (!root.value("observations").isObject())
        {
            errorMessage = "config.json 'observations' must be a JSON object";
            return false;
        }
        const QJsonObject obs = root.value("observations").toObject();

        // save_interval (string, same syntax as runtime.interval)
        const QString saveIntervalQ =
            obs.value("save_interval").toString().trimmed();
        if (!saveIntervalQ.isEmpty())
        {
            QString saveErr;
            const qint64 saveMs =
                parseIntervalMs(saveIntervalQ.toStdString(), saveErr);
            if (saveMs < 0)
            {
                errorMessage =
                    "config.json observations.save_interval error: " + saveErr;
                return false;
            }
            observations.saveIntervalMs = saveMs;
        }

        // noise_sigma (number or object/map, dimensionless)
        // Backward-compatible scalar form:
        //   "noise_sigma": 0.1
        // Per-series override form:
        //   "noise_sigma": {
        //       "default": 0.1,
        //       "soil moisture": 0.05,
        //       "ert": 0.05,
        //       "well_c": 0.02,
        //       "precip": 0.0
        //   }
        // Object keys are lower-case substring patterns matched against the
        // observed-output series name. "default" sets observations.noiseSigma.
        if (obs.contains("noise_sigma"))
        {
            const QJsonValue v = obs.value("noise_sigma");

            if (v.isDouble())
            {
                const double sigma = v.toDouble(0.0);
                if (sigma < 0.0)
                {
                    errorMessage = "config.json observations.noise_sigma must be >= 0 "
                                   "(got " + QString::number(sigma) + ")";
                    return false;
                }
                observations.noiseSigma = sigma;
            }
            else if (v.isObject())
            {
                const QJsonObject obj = v.toObject();
                for (auto it = obj.begin(); it != obj.end(); ++it)
                {
                    if (!it.value().isDouble())
                    {
                        errorMessage =
                            "config.json observations.noise_sigma['" + it.key() +
                            "'] must be a number";
                        return false;
                    }

                    const double sigma = it.value().toDouble(0.0);
                    if (sigma < 0.0)
                    {
                        errorMessage =
                            "config.json observations.noise_sigma['" + it.key() +
                            "'] must be >= 0 (got " + QString::number(sigma) + ")";
                        return false;
                    }

                    QString keyQ = it.key().trimmed().toLower();
                    if (keyQ.isEmpty())
                    {
                        errorMessage =
                            "config.json observations.noise_sigma contains an empty key";
                        return false;
                    }

                    if (keyQ == "default")
                    {
                        observations.noiseSigma = sigma;
                    }
                    else
                    {
                        observations.noiseSigmaByPattern[keyQ.toStdString()] = sigma;
                    }
                }
            }
            else
            {
                errorMessage =
                    "config.json observations.noise_sigma must be a number or object";
                return false;
            }
        }

        // noise_correlation_time (string, same syntax as runtime.interval)
        const QString tauQ =
            obs.value("noise_correlation_time").toString().trimmed();
        if (!tauQ.isEmpty())
        {
            QString tauErr;
            const qint64 tauMs =
                parseIntervalMs(tauQ.toStdString(), tauErr);
            if (tauMs < 0)
            {
                errorMessage =
                    "config.json observations.noise_correlation_time error: "
                    + tauErr;
                return false;
            }
            observations.noiseCorrelationTimeMs = tauMs;
        }
    }

    // ------------------------------------------------------------------
    // assimilation{} (optional; controls observation polling & calibration)
    // ------------------------------------------------------------------
    // Defaults: disabled. The block being absent means the forward twin
    // does not poll any source and does not run calibration.

    if (root.contains("assimilation"))
    {
        if (!root.value("assimilation").isObject())
        {
            errorMessage = "config.json 'assimilation' must be a JSON object";
            return false;
        }
        const QJsonObject as = root.value("assimilation").toObject();

        const QString csvQ  = as.value("truth_csv_url").toString().trimmed();
        const QString metaQ = as.value("truth_meta_url").toString().trimmed();
        const QString pollQ = as.value("poll_interval").toString().trimmed();

        if (csvQ.isEmpty())
        {
            errorMessage = "config.json assimilation.truth_csv_url is required "
                           "when the assimilation block is present";
            return false;
        }
        if (pollQ.isEmpty())
        {
            errorMessage = "config.json assimilation.poll_interval is required "
                           "when the assimilation block is present";
            return false;
        }

        QString pollErr;
        const qint64 pollMs = parseIntervalMs(pollQ.toStdString(), pollErr);
        if (pollMs < 0)
        {
            errorMessage = "config.json assimilation.poll_interval error: " + pollErr;
            return false;
        }

        assimilation.enabled        = true;
        assimilation.truthCsvUrl    = csvQ.toStdString();
        assimilation.truthMetaUrl   = metaQ.toStdString();
        assimilation.pollIntervalMs = pollMs;

        // calibration_output_dir (optional; defaults to "outputs/calibration"
        // under the deployment root). Resolved against deploymentRoot the
        // same way other deployment-relative paths are.
        const QString calDirQ =
            as.value("calibration_output_dir").toString().trimmed();
        const QString calDirResolved = resolvePath(
            calDirQ.isEmpty() ? "outputs/calibration" : calDirQ);
        assimilation.calibrationOutputDir = calDirResolved.toStdString();

        if (as.contains("calibration_observations") &&
            as["calibration_observations"].isArray())
        {
            const QJsonArray arr = as["calibration_observations"].toArray();
            for (const QJsonValue &v : arr)
                if (v.isString())
                    assimilation.calibrationObservations
                        .push_back(v.toString().toStdString());
        }
    }
    // ------------------------------------------------------------------
    // Auto-derived working directories under the deployment root
    // ------------------------------------------------------------------
    const QString rootQ = QString::fromStdString(deploymentRoot);
    stateDir         = QDir(rootQ).absoluteFilePath("state").toStdString();
    outputDir        = QDir(rootQ).absoluteFilePath("outputs").toStdString();
    modelSnapshotDir = QDir(rootQ).absoluteFilePath("snapshots").toStdString();

    for (const auto &dir : { stateDir, outputDir, modelSnapshotDir })
        QDir().mkpath(QString::fromStdString(dir));

    // ------------------------------------------------------------------
    // Sanity-check that the model and viz files exist
    // ------------------------------------------------------------------
    if (!QFileInfo::exists(QString::fromStdString(scriptFile)))
    {
        errorMessage = "model_file does not exist: " + QString::fromStdString(scriptFile);
        return false;
    }
    if (!QFileInfo::exists(QString::fromStdString(vizFile)))
    {
        errorMessage = "viz_file does not exist: " + QString::fromStdString(vizFile);
        return false;
    }

    // ------------------------------------------------------------------
    // Log resolved configuration
    // ------------------------------------------------------------------
    std::cout << "[Config] config.json       : " << configPath.toStdString() << "\n"
              << "[Config] deployment_root   : " << deploymentRoot << "\n"
              << "[Config] deployment_name   : " << deploymentName << "\n"
              << "[Config] port              : " << port << "\n"
              << "[Config] script_file       : " << scriptFile << "\n"
              << "[Config] viz_file          : " << vizFile << "\n"
              << "[Config] state_dir         : " << stateDir << "\n"
              << "[Config] output_dir        : " << outputDir << "\n"
              << "[Config] model_snapshot_dir: " << modelSnapshotDir << "\n"
              << "[Config] weather_source    : " << weatherSource << "\n"
              << "[Config] latitude          : " << latitude << "\n"
              << "[Config] longitude         : " << longitude << "\n"
              << "[Config] interval          : " << intervalStr
              << " (" << intervalMs << " ms)\n";

    if (forecastHorizonMs > 0)
        std::cout << "[Config] forecast_horizon  : " << forecastHorizonStr
                  << " (" << forecastHorizonMs << " ms)\n";
    else
        std::cout << "[Config] forecast_horizon  : disabled\n";

    if (!startDatetime.empty())
        std::cout << "[Config] start_datetime    : " << startDatetime << "\n";

    if (!stopDatetime.empty())
        std::cout << "[Config] stop_datetime     : " << stopDatetime << "\n";

    std::cout << "[Config] time_acceleration : " << timeAcceleration << "x\n";

    if (!loadModelJson.empty())
        std::cout << "[Config] load_model_json   : " << loadModelJson << "\n";

    if (!weatherFile.empty())
        std::cout << "[Config] weather_file      : " << weatherFile << "\n";

    std::cout << "[Config] obs.save_interval : " << observations.saveIntervalMs
              << " ms\n"
              << "[Config] obs.noise_sigma   : " << observations.noiseSigma
              << "\n";

    if (!observations.noiseSigmaByPattern.empty())
    {
        std::cout << "[Config] obs.noise_by_name : ";
        for (const auto &kv : observations.noiseSigmaByPattern)
            std::cout << "'" << kv.first << "'=" << kv.second << " ";
        std::cout << "\n";
    }

    const bool obsNoiseEnabled =
        (observations.noiseSigma > 0.0) || !observations.noiseSigmaByPattern.empty();

    std::cout << "[Config] obs.noise_corr_t  : "
              << observations.noiseCorrelationTimeMs << " ms";
    if (observations.noiseCorrelationTimeMs == 0 && obsNoiseEnabled)
        std::cout << " (white-noise limit)";
    if (!obsNoiseEnabled)
        std::cout << " (noise disabled)";
    std::cout << "\n";

    if (assimilation.enabled)
    {
        std::cout << "[Config] assim.csv_url     : " << assimilation.truthCsvUrl  << "\n"
                  << "[Config] assim.meta_url    : "
                  << (assimilation.truthMetaUrl.empty() ? "(none)" : assimilation.truthMetaUrl)
                  << "\n"
                  << "[Config] assim.poll_int    : "
                  << assimilation.pollIntervalMs << " ms\n";
    }
    else
    {
        std::cout << "[Config] assimilation      : disabled\n";
    }

    std::cout << "[Config] advance_to_obs   : "
              << (advanceToObservations ? "true" : "false") << "\n";

    std::cout << "[Config] keep_debug_outputs: "
              << (keepDebugOutputs ? "true" : "false") << "\n";

    if (assimilation.enabled)
    {
        std::cout << "[Config] calibration_obs : ";
        if (assimilation.calibrationObservations.empty())
            std::cout << "(all matched)";
        else
            for (const auto &name : assimilation.calibrationObservations)
                std::cout << "'" << name << "' ";
        std::cout << "\n";
    }

    return true;
}

// Static helper. Erases the *contents* of dirPath but leaves dirPath itself
// in place. Returns false on the first I/O error encountered. Best-effort
// deletion — partial cleanup is possible if a file can't be removed.
// ---------------------------------------------------------------------------
// DTConfig::eraseDirectoryContents
// Erases all files and subdirectories inside dirPath, but leaves dirPath
// itself in place. Symlinks are removed (not their targets).
// ---------------------------------------------------------------------------
bool DTConfig::eraseDirectoryContents(const QString &dirPath, QString &err)
{
    QDir dir(dirPath);
    if (!dir.exists()) return true;   // nothing to erase

    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::Name);

    for (const QFileInfo &entry : entries)
    {
        const QString path = entry.absoluteFilePath();

        if (entry.isDir() && !entry.isSymLink())
        {
            QDir sub(path);
            if (!sub.removeRecursively())
            {
                err = "failed to remove subdirectory: " + path;
                return false;
            }
        }
        else
        {
            if (!QFile::remove(path))
            {
                err = "failed to remove: " + path;
                return false;
            }
        }
    }
    return true;
}
