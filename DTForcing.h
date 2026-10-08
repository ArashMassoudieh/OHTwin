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

// Forcing map: one series per model source, for models with many forcing sources (a watershed with one
// rainfall and one ET source per sub-catchment) instead of the single "Rain" source of the interpreter
// path. Each entry builds one hourly series from a provider and applies it to one or more model sources:
//
//   "forcing": {
//     "past_days": 3, "forecast_days": 10,          // Open-Meteo window around "now"
//     "entries": [
//       { "name": "rain SC01", "variable": "precipitation", "provider": "openmeteo",
//         "points": [ {"lat": 39.12, "lon": -77.11, "weight": 0.6}, {"lat": 39.13, "lon": -77.10, "weight": 0.4} ],
//         "scale": 1.0,
//         "targets": [ {"source": "P_SC01", "quantity": "timeseries"} ] },
//       { "name": "ET0 SC01", "variable": "et0", "provider": "csv", "file": "forcing/et_sc01.csv",
//         "targets": [ {"source": "ET_SC01", "quantity": "ET_timeseries"},
//                      {"source": "ET2_SC01", "quantity": "ET_timeseries"} ] } ] }
//
// or "forcing": {"file": "forcing_map.json"} with the same object in a separate file.
//
// Variables and units handed to the model:
//   precipitation : depth (m) per interval [start, end]     (OHQ Precipitation source)
//   et0           : rate (m/day) at interval mid-points      (OHQ ET time-series source)
// Providers:
//   openmeteo     : Open-Meteo forecast API, hourly "precipitation" / "et0_fao_evapotranspiration" (mm in the
//                   preceding hour), all points of a cycle in batched multi-location requests, past_days back.
//   csv           : precipitation "start,end,depth_m" rows (OHQ precipitation file) or series "t,value" rows,
//                   times as OHQ day serials. Used for replays and for feeds prepared outside the twin (radar).
#pragma once
#include <QJsonValue>
#include <QString>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct ForcingTarget
{
    std::string source;     // model source name, e.g. "P_SC01"
    std::string quantity;   // its time-series quantity, e.g. "timeseries" or "ET_timeseries"
};

struct ForcingPoint
{
    double lat = 0, lon = 0, weight = 1.0;
};

struct ForcingEntry
{
    std::string name;
    std::string variable;       // "precipitation" | "et0"
    std::string provider;       // "openmeteo" | "csv"
    std::vector<ForcingPoint> points;
    std::string file;           // csv provider (resolved path)
    double scale = 1.0;         // multiplier (bias factor)
    std::vector<ForcingTarget> targets;
    bool isPrecipitation() const { return variable == "precipitation"; }
};

struct ForcingConfig
{
    bool enabled = false;
    int pastDays = 3;
    int forecastDays = 10;
    std::vector<ForcingEntry> entries;
};

// One entry's series over a window. Precipitation: start/end/value (depth m). Series: t/value (m/day).
struct ForcingSeries
{
    std::vector<double> start, end, value;   // precipitation
    std::vector<double> t;                   // series (value shared)
};

namespace DTForcing
{
// Parse the "forcing" object (inline or {"file": ...}); resolve relative paths with `resolve`.
bool parse(const QJsonValue &v, const std::function<QString(const QString &)> &resolve,
           ForcingConfig &cfg, QString &err);

// Build every entry's series over [t0, t1] (OHQ day serials). Returns false if an entry has no data covering
// the window; `err` names the entries. Series are clipped to [t0 - 1 h, t1 + 1 h].
bool fetch(const ForcingConfig &cfg, double t0, double t1, std::vector<ForcingSeries> &out, QString &err);
}
