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

// DTWeather.cpp
#include "DTWeather.h"
#include "noaaweatherfetcher.h"
#include "System.h"
#include "Source.h"
#include <iostream>
#include "Quan.h"

namespace DTWeather
{

CPrecipitation fetchPrecipitation(const std::string &weatherSource,
                                  double             latitude,
                                  double             longitude,
                                  const QDateTime   &intervalStart,
                                  const QDateTime   &intervalEnd)
{
    NOAAWeatherFetcher fetcher;

    if (weatherSource == "openmeteo")
    {
        CPrecipitation precip = fetcher.getOpenMeteoPrecipitation(
            latitude, longitude, intervalStart, intervalEnd);
        if (precip.n == 0)
            std::cerr << "[Weather] Warning: "
                      << fetcher.lastError().toStdString() << "\n";
        return precip;
    }

    if (weatherSource == "openmeteo_historical")
    {
        CPrecipitation precip = fetcher.getOpenMeteoHistoricalPrecipitation(
            latitude, longitude, intervalStart, intervalEnd);
        if (precip.n == 0)
            std::cerr << "[Weather] Warning: "
                      << fetcher.lastError().toStdString() << "\n";
        return precip;
    }

    std::cerr << "[Weather] Unknown weather_source: '" << weatherSource
              << "' (expected 'openmeteo' or 'openmeteo_historical')\n";
    return CPrecipitation();
}

void injectPrecipitation(System *system, const CPrecipitation &precip)
{
    if (precip.n == 0)
    {
        std::cerr << "[Weather] injectPrecipitation: empty precipitation, skipping.\n";
        return;
    }

    Source *src = system->source("Rain");
    if (!src)
    {
        std::cerr << "[Weather] injectPrecipitation: source 'Rain' not found.\n";
        return;
    }

    auto *var = src->Variable("timeseries");
    if (!var)
    {
        std::cerr << "[Weather] injectPrecipitation: variable 'timeseries' not found on 'Rain'.\n";
        return;
    }

    var->SetTimeSeries(precip);
    std::cout << "[Weather] Injected " << precip.n
              << " precipitation bins into 'Rain' source.\n";
}

TimeSeries<double> fetchWeatherVariable(const std::string &weatherSource,
                                        const std::string &quantity,
                                        double             latitude,
                                        double             longitude,
                                        const QDateTime   &intervalStart,
                                        const QDateTime   &intervalEnd)
{
    NOAAWeatherFetcher fetcher;
    const QString q = QString::fromStdString(quantity);

    if (weatherSource == "openmeteo")
    {
        TimeSeries<double> ts = fetcher.getOpenMeteoTimeSeries(
            q, latitude, longitude, intervalStart, intervalEnd);
        if (ts.size() == 0)
            std::cerr << "[Weather] Warning fetching '" << quantity << "': "
                      << fetcher.lastError().toStdString() << "\n";
        return ts;
    }
    if (weatherSource == "openmeteo_historical")
    {
        TimeSeries<double> ts = fetcher.getOpenMeteoHistoricalTimeSeries(
            q, latitude, longitude, intervalStart, intervalEnd);
        if (ts.size() == 0)
            std::cerr << "[Weather] Warning fetching '" << quantity << "': "
                      << fetcher.lastError().toStdString() << "\n";
        return ts;
    }
    std::cerr << "[Weather] Unknown weather_source: '" << weatherSource
              << "' (expected 'openmeteo' or 'openmeteo_historical')\n";
    return TimeSeries<double>();
}

void injectWeather(System                    *system,
                   const std::string         &sourceName,
                   const std::string         &variableName,
                   const TimeSeries<double>  &series)
{
    if (series.size() == 0)
    {
        std::cerr << "[Weather] injectWeather: empty series for '"
                  << sourceName << "." << variableName << "', skipping.\n";
        return;
    }
    Source *src = system->source(sourceName);
    if (!src)
    {
        std::cerr << "[Weather] injectWeather: source '" << sourceName
                  << "' not found, skipping (model may not use this variable).\n";
        return;
    }
    auto *var = src->Variable(variableName);
    if (!var)
    {
        std::cerr << "[Weather] injectWeather: variable '" << variableName
                  << "' not found on '" << sourceName << "', skipping.\n";
        return;
    }
    var->SetTimeSeries(series);
    std::cout << "[Weather] Injected " << series.size()
              << " samples into '" << sourceName << "." << variableName
              << "'.\n";
}

}   // namespace DTWeather
