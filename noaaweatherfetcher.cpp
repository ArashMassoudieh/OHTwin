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

#include "noaaweatherfetcher.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <iostream>

// OHQ epoch: day-serial 0 = 1899-12-30 (Excel convention)
static const QDate kNOAAOHQEpoch(1899, 12, 30);

// ---------------------------------------------------------------------------
// warnIfWindowUncovered
// ---------------------------------------------------------------------------
// Open-Meteo's forecast_days counts whole CALENDAR days from today 00:00 UTC,
// not days from "now". forecast_days=7 therefore reaches only 6.96 days ahead
// of a cycle starting at 00:00, and 6.00 days ahead of one starting at 23:00 —
// while a forecast stage simulates [t, t+interval+horizon], eight days for the
// JM deployment. The tail then has no data behind it: the series simply stops,
// and downstream interpolation holds the final sample, so invented weather is
// silently presented as forecast. forecast_days is now 10 (about 9.0-9.96 days
// of coverage) to keep an 8-day window comfortably inside real data.
//
// This warns if that margin is ever exhausted anyway — a longer horizon, a
// longer interval, or an Open-Meteo change would all erode it quietly.
static void warnIfWindowUncovered(const std::string &what,
                                  const QDateTime &dataEnd,
                                  const QDateTime &intervalEnd)
{
    if (!dataEnd.isValid() || !intervalEnd.isValid() || dataEnd >= intervalEnd)
        return;

    const double shortHours = dataEnd.msecsTo(intervalEnd) / 3600000.0;
    std::cerr << "[OpenMeteo] WARNING: '" << what << "' data ends "
              << dataEnd.toUTC().toString(Qt::ISODate).toStdString()
              << " but the simulated window runs to "
              << intervalEnd.toUTC().toString(Qt::ISODate).toStdString()
              << " (short by " << shortHours << " h). Values past the data end "
                 "are held at the last sample, not forecast — raise "
                 "forecast_days or shorten forecast_horizon.\n";
}

NOAAWeatherFetcher::NOAAWeatherFetcher(QObject *parent)
    : QObject(parent)
    , manager(new QNetworkAccessManager(this))
{}

// ---------------------------------------------------------------------------
// toOHQDaySerial
// ---------------------------------------------------------------------------
double NOAAWeatherFetcher::toOHQDaySerial(const QDateTime &dt)
{
    const qint64 ms = QDateTime(kNOAAOHQEpoch, QTime(0,0,0), Qt::UTC)
    .msecsTo(dt.toUTC());
    return static_cast<double>(ms) / (86400.0 * 1000.0);
}

// ---------------------------------------------------------------------------
// parseDurationSecs
// Parses ISO 8601 duration: P[nD][T[nH][nM][nS]]
// Examples: PT1H → 3600   P1D → 86400   P7DT14H → 655200   PT30M → 1800
// ---------------------------------------------------------------------------
qint64 NOAAWeatherFetcher::parseDurationSecs(const QString &dur)
{
    qint64 secs = 0;

    const int tIdx = dur.indexOf('T');
    const QString datePart = (tIdx == -1) ? dur : dur.left(tIdx);
    const QString timePart = (tIdx == -1) ? QString() : dur.mid(tIdx + 1);

    QRegularExpression dayRx("(\\d+)D");
    QRegularExpression hourRx("(\\d+)H");
    QRegularExpression minRx("(\\d+)M");
    QRegularExpression secRx("(\\d+)S");

    auto match = dayRx.match(datePart);
    if (match.hasMatch())
        secs += match.captured(1).toLongLong() * 86400;

    match = hourRx.match(timePart);
    if (match.hasMatch())
        secs += match.captured(1).toLongLong() * 3600;

    match = minRx.match(timePart);
    if (match.hasMatch())
        secs += match.captured(1).toLongLong() * 60;

    match = secRx.match(timePart);
    if (match.hasMatch())
        secs += match.captured(1).toLongLong();

    return secs;
}

// ---------------------------------------------------------------------------
// getWeatherPrediction  (NOAA gridpoints)
// ---------------------------------------------------------------------------
QVector<WeatherData> NOAAWeatherFetcher::getWeatherPrediction(
    const QString &office, int gridX, int gridY, datatype type)
{
    QVector<WeatherData> result;

    const QString url = QString("https://api.weather.gov/gridpoints/%1/%2,%3")
                            .arg(office).arg(gridX).arg(gridY);

    QNetworkRequest request((QUrl(url)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", "DTRunner/1.0 (openhydroqual@example.com)");

    QString dataTypeKey;
    switch (type) {
    case datatype::PrecipitationAmount:        dataTypeKey = "quantitativePrecipitation"; break;
    case datatype::ProbabilityofPrecipitation: dataTypeKey = "probabilityOfPrecipitation"; break;
    case datatype::RelativeHumidity:           dataTypeKey = "relativeHumidity"; break;
    case datatype::Temperature:                dataTypeKey = "temperature"; break;
    }

    request.setTransferTimeout(30000);   // 30 s: fail a hung fetch instead of blocking the event loop forever
    QNetworkReply *reply = manager->get(request);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        std::cerr << "[NOAA] Network error: "
                  << reply->errorString().toStdString() << "\n";
        reply->deleteLater();
        return result;
    }

    const QJsonObject root =
        QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();

    const QJsonArray values =
        root["properties"].toObject()[dataTypeKey].toObject()["values"].toArray();

    if (values.isEmpty()) {
        std::cerr << "[NOAA] No values found for key: "
                  << dataTypeKey.toStdString() << "\n";
        return result;
    }

    for (const auto &entry : values) {
        const QJsonObject obj = entry.toObject();
        const QString validTime = obj["validTime"].toString();

        const QStringList parts = validTime.split('/');
        if (parts.size() != 2) continue;

        const QDateTime start =
            QDateTime::fromString(parts[0], Qt::ISODate).toUTC();
        if (!start.isValid()) continue;

        const qint64 durSecs = parseDurationSecs(parts[1]);
        if (durSecs <= 0) continue;

        const QDateTime end = start.addSecs(durSecs);
        const double value  = obj["value"].toDouble();

        result.push_back({ start, end, value });
    }

    std::cout << "[NOAA] Fetched " << result.size()
              << " " << dataTypeKey.toStdString() << " records\n";
    return result;
}

// ---------------------------------------------------------------------------
// getOpenMeteoPrecipitation
// Fetches hourly precipitation from Open-Meteo (free, no API key).
// Returns CPrecipitation bins in OHQ day-serial units filtered to interval.
// ---------------------------------------------------------------------------
CPrecipitation NOAAWeatherFetcher::getOpenMeteoPrecipitation(
    double latitude, double longitude,
    const QDateTime &intervalStart,
    const QDateTime &intervalEnd)
{
    CPrecipitation precip;
    m_lastError.clear();

    // Build URL
    QUrl url("https://api.open-meteo.com/v1/forecast");
    QUrlQuery query;
    query.addQueryItem("latitude",      QString::number(latitude,  'f', 5));
    query.addQueryItem("longitude",     QString::number(longitude, 'f', 5));
    query.addQueryItem("hourly",        "precipitation");
    query.addQueryItem("forecast_days", "10");   // see coverage note above warnIfWindowUncovered()
    query.addQueryItem("timezone",      "GMT");
    url.setQuery(query);

    std::cout << "[OpenMeteo] Fetching: " << url.toString().toStdString() << "\n";

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent",
                         "DTRunner/1.0 (openhydroqual@example.com)");

    request.setTransferTimeout(30000);   // 30 s: fail a hung fetch instead of blocking the event loop forever
    QNetworkReply *reply = manager->get(request);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        m_lastError = "[OpenMeteo] Network error: " + reply->errorString();
        std::cerr << m_lastError.toStdString() << "\n";
        reply->deleteLater();
        return precip;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
    reply->deleteLater();

    if (doc.isNull()) {
        m_lastError = "[OpenMeteo] JSON parse error: " + parseError.errorString();
        std::cerr << m_lastError.toStdString() << "\n";
        return precip;
    }

    const QJsonObject root   = doc.object();
    const QJsonObject hourly = root.value("hourly").toObject();
    const QJsonArray  times  = hourly.value("time").toArray();
    const QJsonArray  values = hourly.value("precipitation").toArray();

    if (times.isEmpty() || times.size() != values.size()) {
        m_lastError = "[OpenMeteo] Unexpected response structure";
        std::cerr << m_lastError.toStdString() << "\n";
        return precip;
    }

    int loaded  = 0;
    int skipped = 0;
    QDateTime dataEnd;   // latest bin the API actually returned

    for (int i = 0; i < times.size(); ++i)
    {
        // Open-Meteo returns "2026-04-15T00:00" — always UTC (timezone=GMT)
        QDateTime binStart = QDateTime::fromString(times[i].toString() + "Z", Qt::ISODate);

        if (!binStart.isValid())
            continue;

        const QDateTime binEnd = binStart.addSecs(3600); // 1-hour bins

        // Track coverage over every returned bin, not just the kept ones.
        if (!dataEnd.isValid() || binEnd > dataEnd)
            dataEnd = binEnd;

        // Skip bins entirely outside the interval window
        if (binEnd <= intervalStart || binStart >= intervalEnd) {
            ++skipped;
            continue;
        }

        // Clamp bin edges to interval window
        const QDateTime clampedStart = qMax(binStart, intervalStart);
        const QDateTime clampedEnd   = qMin(binEnd,   intervalEnd);

        const double s  = toOHQDaySerial(clampedStart);
        const double e  = toOHQDaySerial(clampedEnd);
        const double mm = values[i].toDouble();

        // Convert mm → metres (OHQ uses SI units)
        precip.append(s, e, mm / 1000.0);
        ++loaded;
    }

    std::cout << "[OpenMeteo] Precipitation bins loaded: " << loaded
              << " (skipped " << skipped << " outside window)\n";
    warnIfWindowUncovered("precipitation", dataEnd, intervalEnd);

    return precip;
}

// ---------------------------------------------------------------------------
// getOpenMeteoHistoricalPrecipitation
// Fetches hourly precipitation from Open-Meteo's archive endpoint
// (ERA5-backed, no API key). Same response shape as the forecast endpoint;
// the only differences are the URL and the use of start_date / end_date
// (YYYY-MM-DD) instead of forecast_days.
// ---------------------------------------------------------------------------
CPrecipitation NOAAWeatherFetcher::getOpenMeteoHistoricalPrecipitation(
    double latitude, double longitude,
    const QDateTime &intervalStart,
    const QDateTime &intervalEnd)
{
    CPrecipitation precip;
    m_lastError.clear();

    // The archive API expects whole-day start_date / end_date in UTC.
    // Pad by one day on each side so we definitely cover all hourly bins
    // that overlap the requested window after clamping.
    const QString startDate =
        intervalStart.toUTC().date().addDays(-1).toString("yyyy-MM-dd");
    const QString endDate =
        intervalEnd.toUTC().date().addDays(1).toString("yyyy-MM-dd");

    QUrl url("https://archive-api.open-meteo.com/v1/archive");
    QUrlQuery query;
    query.addQueryItem("latitude",   QString::number(latitude,  'f', 5));
    query.addQueryItem("longitude",  QString::number(longitude, 'f', 5));
    query.addQueryItem("hourly",     "precipitation");
    query.addQueryItem("start_date", startDate);
    query.addQueryItem("end_date",   endDate);
    query.addQueryItem("timezone",   "GMT");
    url.setQuery(query);

    std::cout << "[OpenMeteo-Archive] Fetching: "
              << url.toString().toStdString() << "\n";

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent",
                         "DTRunner/1.0 (openhydroqual@example.com)");

    request.setTransferTimeout(30000);   // 30 s: fail a hung fetch instead of blocking the event loop forever
    QNetworkReply *reply = manager->get(request);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        m_lastError = "[OpenMeteo-Archive] Network error: " + reply->errorString();
        std::cerr << m_lastError.toStdString() << "\n";
        reply->deleteLater();
        return precip;
    }

    QJsonParseError parseError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(reply->readAll(), &parseError);
    reply->deleteLater();

    if (doc.isNull()) {
        m_lastError = "[OpenMeteo-Archive] JSON parse error: " + parseError.errorString();
        std::cerr << m_lastError.toStdString() << "\n";
        return precip;
    }

    const QJsonObject root   = doc.object();
    const QJsonObject hourly = root.value("hourly").toObject();
    const QJsonArray  times  = hourly.value("time").toArray();
    const QJsonArray  values = hourly.value("precipitation").toArray();

    if (times.isEmpty() || times.size() != values.size()) {
        m_lastError = "[OpenMeteo-Archive] Unexpected response structure";
        std::cerr << m_lastError.toStdString() << "\n";
        return precip;
    }

    int loaded  = 0;
    int skipped = 0;

    for (int i = 0; i < times.size(); ++i)
    {
        QDateTime binStart = QDateTime::fromString(
            times[i].toString(), "yyyy-MM-ddTHH:mm");
        binStart.setTimeSpec(Qt::UTC);
        if (!binStart.isValid()) continue;

        const QDateTime binEnd = binStart.addSecs(3600);

        if (binEnd <= intervalStart || binStart >= intervalEnd) {
            ++skipped;
            continue;
        }

        const QDateTime clampedStart = qMax(binStart, intervalStart);
        const QDateTime clampedEnd   = qMin(binEnd,   intervalEnd);

        const double s  = toOHQDaySerial(clampedStart);
        const double e  = toOHQDaySerial(clampedEnd);
        const double mm = values[i].toDouble();

        precip.append(s, e, mm / 1000.0);
        ++loaded;
    }

    std::cout << "[OpenMeteo-Archive] Precipitation bins loaded: " << loaded
              << " (skipped " << skipped << " outside window)\n";

    return precip;
}

// ---------------------------------------------------------------------------
// getOpenMeteoTimeSeries
// Forecast endpoint, single hourly variable (temperature, RH, wind, radiation).
// Returns point-sampled TimeSeries<double> in OHQ day-serial time.
// ---------------------------------------------------------------------------
TimeSeries<double> NOAAWeatherFetcher::getOpenMeteoTimeSeries(
    const QString   &quantity,
    double           latitude,
    double           longitude,
    const QDateTime &intervalStart,
    const QDateTime &intervalEnd)
{
    TimeSeries<double> ts;
    m_lastError.clear();

    QUrl url("https://api.open-meteo.com/v1/forecast");
    QUrlQuery query;
    query.addQueryItem("latitude",      QString::number(latitude,  'f', 5));
    query.addQueryItem("longitude",     QString::number(longitude, 'f', 5));
    query.addQueryItem("hourly",        quantity);
    query.addQueryItem("forecast_days", "10");   // see coverage note above warnIfWindowUncovered()
    query.addQueryItem("timezone",      "GMT");
    url.setQuery(query);

    std::cout << "[OpenMeteo] Fetching: "
              << url.toString().toStdString() << "\n";

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent",
                         "DTRunner/1.0 (openhydroqual@example.com)");

    request.setTransferTimeout(30000);   // 30 s: fail a hung fetch instead of blocking the event loop forever
    QNetworkReply *reply = manager->get(request);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        m_lastError = "[OpenMeteo] Network error: " + reply->errorString();
        std::cerr << m_lastError.toStdString() << "\n";
        reply->deleteLater();
        return ts;
    }

    QJsonParseError parseError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(reply->readAll(), &parseError);
    reply->deleteLater();

    if (doc.isNull()) {
        m_lastError = "[OpenMeteo] JSON parse error: " + parseError.errorString();
        std::cerr << m_lastError.toStdString() << "\n";
        return ts;
    }

    const QJsonObject root   = doc.object();
    const QJsonObject hourly = root.value("hourly").toObject();
    const QJsonArray  times  = hourly.value("time").toArray();
    const QJsonArray  values = hourly.value(quantity).toArray();

    if (times.isEmpty() || times.size() != values.size()) {
        m_lastError = "[OpenMeteo] Unexpected response structure for '"
                      + quantity + "'";
        std::cerr << m_lastError.toStdString() << "\n";
        return ts;
    }

    int loaded  = 0;
    int skipped = 0;
    QDateTime dataEnd;   // latest sample the API actually returned

    for (int i = 0; i < times.size(); ++i)
    {
        QDateTime t = QDateTime::fromString(times[i].toString() + "Z", Qt::ISODate);
        if (!t.isValid())
            continue;

        // Track coverage over every returned sample, not just the kept ones.
        if (!dataEnd.isValid() || t > dataEnd)
            dataEnd = t;

        if (t < intervalStart || t > intervalEnd) {
            ++skipped;
            continue;
        }

        const double tSerial = toOHQDaySerial(t);
        const double v       = values[i].toDouble();
        ts.append(tSerial, v);
        ++loaded;
    }

    ts.setName(quantity.toStdString());
    std::cout << "[OpenMeteo] '" << quantity.toStdString()
              << "' samples loaded: " << loaded
              << " (skipped " << skipped << " outside window)\n";
    warnIfWindowUncovered(quantity.toStdString(), dataEnd, intervalEnd);

    return ts;
}

// ---------------------------------------------------------------------------
// getOpenMeteoHistoricalTimeSeries
// Archive (ERA5) endpoint sibling of getOpenMeteoTimeSeries.
// ---------------------------------------------------------------------------
TimeSeries<double> NOAAWeatherFetcher::getOpenMeteoHistoricalTimeSeries(
    const QString   &quantity,
    double           latitude,
    double           longitude,
    const QDateTime &intervalStart,
    const QDateTime &intervalEnd)
{
    TimeSeries<double> ts;
    m_lastError.clear();

    const QString startDate =
        intervalStart.toUTC().date().addDays(-1).toString("yyyy-MM-dd");
    const QString endDate =
        intervalEnd.toUTC().date().addDays(1).toString("yyyy-MM-dd");

    QUrl url("https://archive-api.open-meteo.com/v1/archive");
    QUrlQuery query;
    query.addQueryItem("latitude",   QString::number(latitude,  'f', 5));
    query.addQueryItem("longitude",  QString::number(longitude, 'f', 5));
    query.addQueryItem("hourly",     quantity);
    query.addQueryItem("start_date", startDate);
    query.addQueryItem("end_date",   endDate);
    query.addQueryItem("timezone",   "GMT");
    url.setQuery(query);

    std::cout << "[OpenMeteo-Archive] Fetching: "
              << url.toString().toStdString() << "\n";

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent",
                         "DTRunner/1.0 (openhydroqual@example.com)");

    request.setTransferTimeout(30000);   // 30 s: fail a hung fetch instead of blocking the event loop forever
    QNetworkReply *reply = manager->get(request);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        m_lastError = "[OpenMeteo-Archive] Network error: " + reply->errorString();
        std::cerr << m_lastError.toStdString() << "\n";
        reply->deleteLater();
        return ts;
    }

    QJsonParseError parseError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(reply->readAll(), &parseError);
    reply->deleteLater();

    if (doc.isNull()) {
        m_lastError = "[OpenMeteo-Archive] JSON parse error: " + parseError.errorString();
        std::cerr << m_lastError.toStdString() << "\n";
        return ts;
    }

    const QJsonObject root   = doc.object();
    const QJsonObject hourly = root.value("hourly").toObject();
    const QJsonArray  times  = hourly.value("time").toArray();
    const QJsonArray  values = hourly.value(quantity).toArray();

    if (times.isEmpty() || times.size() != values.size()) {
        m_lastError = "[OpenMeteo-Archive] Unexpected response structure for '"
                      + quantity + "'";
        std::cerr << m_lastError.toStdString() << "\n";
        return ts;
    }

    int loaded  = 0;
    int skipped = 0;

    for (int i = 0; i < times.size(); ++i)
    {
        QDateTime t = QDateTime::fromString(
            times[i].toString(), "yyyy-MM-ddTHH:mm");
        t.setTimeSpec(Qt::UTC);
        if (!t.isValid())
            continue;

        if (t < intervalStart || t > intervalEnd) {
            ++skipped;
            continue;
        }

        const double tSerial = toOHQDaySerial(t);
        const double v       = values[i].toDouble();
        ts.append(tSerial, v);
        ++loaded;
    }

    ts.setName(quantity.toStdString());
    std::cout << "[OpenMeteo-Archive] '" << quantity.toStdString()
              << "' samples loaded: " << loaded
              << " (skipped " << skipped << " outside window)\n";

    return ts;
}
