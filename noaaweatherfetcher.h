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
#include "Precipitation.h"
#include <QDateTime>
#include <QObject>
#include <QVector>
class QNetworkAccessManager;
enum class datatype {
    ProbabilityofPrecipitation,
    Temperature,
    PrecipitationAmount,
    RelativeHumidity
};
struct WeatherData {
    QDateTime startTime;   // bin start (was: timestamp)
    QDateTime endTime;     // bin end (startTime + duration)
    double    value;
};
class NOAAWeatherFetcher : public QObject {
    Q_OBJECT
public:
    explicit NOAAWeatherFetcher(QObject *parent = nullptr);

    // NOAA gridpoints forecast (6-hour bins)
    // office: e.g. "LWX",  gridX/gridY: integer grid coords
    QVector<WeatherData> getWeatherPrediction(
        const QString &office, int gridX, int gridY, datatype type);

    // Open-Meteo hourly precipitation forecast (1-hour bins, no API key needed)
    // Returns CPrecipitation in OHQ day-serial units, filtered to [intervalStart, intervalEnd]
    CPrecipitation getOpenMeteoPrecipitation(
        double latitude, double longitude,
        const QDateTime &intervalStart,
        const QDateTime &intervalEnd);

    // Open-Meteo hourly precipitation **archive** (historical) — same units
    // and bin convention as getOpenMeteoPrecipitation, but pulls from the
    // ERA5-backed archive endpoint. Suitable for Truth-Twin / historical
    // replay runs. Note: archive data is delayed ~5 days from real time.
    CPrecipitation getOpenMeteoHistoricalPrecipitation(
        double latitude, double longitude,
        const QDateTime &intervalStart,
        const QDateTime &intervalEnd);


    // Open-Meteo hourly forecast for a single non-precipitation variable
    // (e.g. "temperature_2m", "relative_humidity_2m", "windspeed_10m",
    // "shortwave_radiation"). Returns a TimeSeries<double> in OHQ day-serial
    // time units, point-sampled at each hourly timestamp returned by the API,
    // filtered to [intervalStart, intervalEnd]. Units pass through unchanged
    // (°C, %, m/s, W/m² — Open-Meteo defaults).
    TimeSeries<double> getOpenMeteoTimeSeries(
        const QString   &quantity,
        double           latitude,
        double           longitude,
        const QDateTime &intervalStart,
        const QDateTime &intervalEnd);

    // Open-Meteo hourly **archive** (ERA5-backed) sibling of
    // getOpenMeteoTimeSeries. Same units and time convention.
    // Note: archive data is delayed ~5 days from real time.
    TimeSeries<double> getOpenMeteoHistoricalTimeSeries(
        const QString   &quantity,
        double           latitude,
        double           longitude,
        const QDateTime &intervalStart,
        const QDateTime &intervalEnd);

    // Last error message from getOpenMeteoPrecipitation
    QString lastError() const { return m_lastError; }

private:
    static qint64 parseDurationSecs(const QString &duration);
    static double toOHQDaySerial(const QDateTime &dt);
    QNetworkAccessManager *manager;
    QString m_lastError;
};
