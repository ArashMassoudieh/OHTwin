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

// DTViewerWriter: the files a watershed viewer (OpenWatershedTwin) reads, written every cycle of a codegen
// deployment (config.json "viewer"; data contract: OpenWatershedTwin docs/viewer_data.md).
//
// The watershed's viewer_config.json says what to write: per unit (sub-catchment) and variable a kernel output
// ("source", with {unit} and {segment} placeholders and '*' globs), an optional divisor ("divide_by") and a
// "scale"; per gage the same for each series plus an observed file; "rain_source" names the model rain source
// of a unit (its forcing entry gives the rainfall chart). The kernel must be generated with those outputs
// (ohq_generate --outputs; OpenWatershedTwin tools/twin_outputs.py spec writes the list).
//
// History: the values of each Advance stage (on the chart time grid) are appended to a rolling store
// (viewer_history.bin) that keeps output_times.history_days; the files combine it with the Forecast stage.
// All files are replaced atomically (QSaveFile), so the viewer never reads a half-written one.
#pragma once
#include "DTConfig.h"
#include "DTForcing.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

class DTViewerWriter
{
public:
    // Reads viewer_config.json and its units layer, binds the variables to the kernel outputs, copies the
    // config and the GIS layers to the web folder, loads the history store.
    bool init(const ViewerOutputConfig &cfg, const std::vector<std::string> &outputNames,
              const ForcingConfig &forcing, QString &err);

    double stepDays() const { return chartStep_; }        // chart time grid (kernel output sampling)
    double historyDays() const { return historyDays_; }

    // Append an Advance stage (kernel outputs on the chart grid; the forcing series of the cycle) to the history.
    void addHistory(const std::vector<double> &t, const std::vector<std::vector<double>> &rows,
                    const std::vector<ForcingSeries> &series);

    // Write the viewer files for "now" (the Advance end) with the Forecast stage after it.
    bool write(double now, const std::vector<double> &t, const std::vector<std::vector<double>> &rows,
               const std::vector<ForcingSeries> &series, const QJsonObject &status, QString &err);

    QStringList warnings() const { return warnings_; }

private:
    struct Channel                     // one value per time: a unit variable, a unit's rain, a gage series
    {
        QString key;                   // "<unit>|<element>:<variable>", "<unit>|rain", "gage|<id>|<series>"
        int out = -1, div = -1;        // kernel output indices (div -1: none)
        double scale = 1;
        int rainEntry = -1;            // rain: forcing entry index
    };
    std::vector<double> values(const Channel &c, const std::vector<double> &t,
                               const std::vector<std::vector<double>> &rows,
                               const std::vector<ForcingSeries> &series) const;
    bool loadHistory();
    bool saveHistory() const;
    static bool saveText(const QString &path, const QByteArray &data, QString &err);

    ViewerOutputConfig cfg_;
    QJsonObject vc_;                   // viewer_config.json
    QStringList units_;
    std::vector<Channel> channels_;
    QHash<QString, int> channelIndex_;
    double chartStep_ = 1.0 / 24, mapStep_ = 3.0 / 24, historyDays_ = 45;
    // history store
    std::vector<double> histT_;
    std::vector<std::vector<float>> histV_;   // [channel][time]
    QStringList warnings_;
};
