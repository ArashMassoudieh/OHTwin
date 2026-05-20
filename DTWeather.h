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

// DTWeather.h
#pragma once

#include <QDateTime>
#include <string>
#include "Precipitation.h"
#include "TimeSeries.h"
class System;

namespace DTWeather
{
CPrecipitation fetchPrecipitation(
    const std::string &weatherSource,
    double             latitude,
    double             longitude,
    const QDateTime   &intervalStart,
    const QDateTime   &intervalEnd);

void injectPrecipitation(System *system, const CPrecipitation &precip);

// Fetch a single hourly weather variable (temperature, RH, wind, radiation, ...)
// from Open-Meteo. `weatherSource` selects forecast vs archive
// ("openmeteo" | "openmeteo_historical"). `quantity` is the Open-Meteo hourly
// variable name, passed through unchanged: "temperature_2m",
// "relative_humidity_2m", "windspeed_10m", "shortwave_radiation", ...
// Returns an empty series on failure (a warning is printed).
TimeSeries<double> fetchWeatherVariable(const std::string &weatherSource,
                                        const std::string &quantity,
                                        double             latitude,
                                        double             longitude,
                                        const QDateTime   &intervalStart,
                                        const QDateTime   &intervalEnd);

// Inject a time series into a named variable on a named Source object.
// Used for Penman ET inputs: e.g. sourceName="Evapotranspiration_Penman (Soil)",
// variableName="Temperature" | "R_h" | "wind_speed" | "solar_radiation".
// Warns (does not error) if the source or variable is missing, so a model
// that uses a different ET formulation runs without modification.
void injectWeather(System                    *system,
                   const std::string         &sourceName,
                   const std::string         &variableName,
                   const TimeSeries<double>  &series);
}
