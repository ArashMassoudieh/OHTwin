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
#include "DTForcing.h"

#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextStream>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
const QDate kOHQEpoch(1899, 12, 30);
const double kHour = 1.0 / 24.0;

double toSerial(const QDateTime &dt)
{
    const QDateTime epoch(kOHQEpoch, QTime(0, 0), QTimeZone::UTC);
    return epoch.msecsTo(dt.toUTC()) / 86400000.0;
}

// Open-Meteo hourly variable name for a forcing variable.
QString openMeteoName(const std::string &variable)
{
    if (variable == "precipitation") return "precipitation";
    if (variable == "et0")           return "et0_fao_evapotranspiration";
    return QString();
}

// Hourly values (mm in the hour ENDING at the time stamp) at one location.
struct HourlyPoint
{
    std::vector<double> tEnd;   // OHQ serial, end of each hour
    std::vector<double> mm;
};

QString pointKey(double lat, double lon)
{
    return QString::number(lat, 'f', 4) + "," + QString::number(lon, 'f', 4);
}

// One multi-location request. Returns false on a network or parse error.
bool requestOpenMeteo(const std::vector<std::pair<double, double>> &pts, const QString &var, int pastDays,
                      int forecastDays, std::vector<HourlyPoint> &out, QString &err)
{
    QStringList lats, lons;
    for (const auto &p : pts) { lats << QString::number(p.first, 'f', 4); lons << QString::number(p.second, 'f', 4); }
    QUrl url("https://api.open-meteo.com/v1/forecast");
    QUrlQuery q;
    q.addQueryItem("latitude", lats.join(","));
    q.addQueryItem("longitude", lons.join(","));
    q.addQueryItem("hourly", var);
    q.addQueryItem("past_days", QString::number(pastDays));
    q.addQueryItem("forecast_days", QString::number(forecastDays));
    q.addQueryItem("timezone", "GMT");
    url.setQuery(q);

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    QNetworkReply *reply = nam.get(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(60000);
    loop.exec();
    if (!reply->isFinished())
    {
        reply->abort();
        reply->deleteLater();
        err = "Open-Meteo request timed out";
        return false;
    }
    const QByteArray body = reply->readAll();
    const bool netOk = reply->error() == QNetworkReply::NoError;
    const QString netErr = reply->errorString();
    reply->deleteLater();
    if (!netOk)
    {
        err = "Open-Meteo: " + netErr + " " + QString::fromUtf8(body.left(300));
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonArray arr;
    if (doc.isArray()) arr = doc.array();          // several locations
    else if (doc.isObject()) arr.append(doc.object());
    if (arr.size() != static_cast<int>(pts.size()))
    {
        err = QString("Open-Meteo returned %1 locations for %2 requested").arg(arr.size()).arg(pts.size());
        return false;
    }
    out.assign(pts.size(), HourlyPoint());
    for (int i = 0; i < arr.size(); ++i)
    {
        const QJsonObject hourly = arr[i].toObject().value("hourly").toObject();
        const QJsonArray times = hourly.value("time").toArray();
        const QJsonArray vals = hourly.value(var).toArray();
        for (int k = 0; k < times.size() && k < vals.size(); ++k)
        {
            if (vals[k].isNull()) continue;
            const QDateTime t = QDateTime::fromString(times[k].toString(), "yyyy-MM-ddTHH:mm");
            QDateTime tu(t.date(), t.time(), QTimeZone::UTC);
            out[i].tEnd.push_back(toSerial(tu));
            out[i].mm.push_back(std::max(0.0, vals[k].toDouble()));
        }
    }
    return true;
}

bool readCsv(const std::string &path, bool precipitation, ForcingSeries &s, QString &err)
{
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        err = "cannot open " + QString::fromStdString(path);
        return false;
    }
    QTextStream in(&f);
    while (!in.atEnd())
    {
        const QStringList c = in.readLine().split(',');
        bool ok0 = false, ok1 = false, ok2 = false;
        if (precipitation && c.size() >= 3)
        {
            const double a = c[0].toDouble(&ok0), b = c[1].toDouble(&ok1), d = c[2].toDouble(&ok2);
            if (ok0 && ok1 && ok2) { s.start.push_back(a); s.end.push_back(b); s.value.push_back(d); }
        }
        else if (!precipitation && c.size() >= 2)
        {
            const double a = c[0].toDouble(&ok0), v = c[1].toDouble(&ok1);
            if (ok0 && ok1) { s.t.push_back(a); s.value.push_back(v); }
        }
    }
    return true;
}

// Keep the rows that touch [a, b] plus one row on each side, so that series coarser than the window
// (daily ET) still bracket it for interpolation.
void clip(ForcingSeries &s, bool precipitation, double a, double b)
{
    const size_t n = s.value.size();
    size_t i0 = n, i1 = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double lo = precipitation ? s.start[i] : s.t[i];
        const double hi = precipitation ? s.end[i] : s.t[i];
        if (hi >= a && lo <= b) { i0 = std::min(i0, i); i1 = std::max(i1, i); }
    }
    if (i0 == n)
    {
        // nothing inside: keep the bracketing pair, if any
        for (size_t i = 0; i + 1 < n; ++i)
            if ((precipitation ? s.end[i] : s.t[i]) < a && (precipitation ? s.start[i + 1] : s.t[i + 1]) > b)
            { i0 = i; i1 = i + 1; }
        if (i0 == n) { s = ForcingSeries(); return; }
    }
    if (i0 > 0) --i0;
    if (i1 + 1 < n) ++i1;
    ForcingSeries o;
    for (size_t i = i0; i <= i1; ++i)
    {
        if (precipitation) { o.start.push_back(s.start[i]); o.end.push_back(s.end[i]); }
        else o.t.push_back(s.t[i]);
        o.value.push_back(s.value[i]);
    }
    s = o;
}

// Coverage of [t0, t1]: the series must reach both ends (within an hour).
bool covers(const ForcingSeries &s, bool precipitation, double t0, double t1)
{
    if (s.value.empty()) return false;
    const double lo = precipitation ? s.start.front() : s.t.front();
    const double hi = precipitation ? s.end.back() : s.t.back();
    return lo <= t0 + kHour && hi >= t1 - kHour;
}
} // namespace

namespace DTForcing
{
bool parse(const QJsonValue &v, const std::function<QString(const QString &)> &resolve, ForcingConfig &cfg,
           QString &err)
{
    QJsonObject o = v.toObject();
    if (o.contains("file"))
    {
        const QString path = resolve(o.value("file").toString());
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
        {
            err = "forcing.file: cannot open " + path;
            return false;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject())
        {
            err = "forcing.file: not a JSON object: " + path;
            return false;
        }
        o = doc.object();
    }
    cfg.pastDays = o.value("past_days").toInt(3);
    cfg.forecastDays = o.value("forecast_days").toInt(10);
    const QJsonArray entries = o.value("entries").toArray();
    for (const QJsonValue &ev : entries)
    {
        const QJsonObject e = ev.toObject();
        ForcingEntry fe;
        fe.name = e.value("name").toString().toStdString();
        fe.variable = e.value("variable").toString().toStdString();
        fe.provider = e.value("provider").toString("openmeteo").toStdString();
        fe.scale = e.value("scale").toDouble(1.0);
        if (fe.variable != "precipitation" && fe.variable != "et0")
        {
            err = QString("forcing entry '%1': variable must be 'precipitation' or 'et0'").arg(fe.name.c_str());
            return false;
        }
        if (fe.provider == "csv")
        {
            if (e.contains("files"))
                for (const QJsonValue &f : e.value("files").toArray())
                    fe.files.push_back(resolve(f.toString()).toStdString());
            else
                fe.files.push_back(resolve(e.value("file").toString()).toStdString());
        }
        else if (fe.provider == "openmeteo")
        {
            for (const QJsonValue &pv : e.value("points").toArray())
            {
                const QJsonObject p = pv.toObject();
                fe.points.push_back({p.value("lat").toDouble(), p.value("lon").toDouble(),
                                     p.value("weight").toDouble(1.0)});
            }
            if (fe.points.empty())
            {
                err = QString("forcing entry '%1': openmeteo needs points").arg(fe.name.c_str());
                return false;
            }
        }
        else
        {
            err = QString("forcing entry '%1': unknown provider '%2'").arg(fe.name.c_str(), fe.provider.c_str());
            return false;
        }
        for (const QJsonValue &tv : e.value("targets").toArray())
        {
            const QJsonObject t = tv.toObject();
            fe.targets.push_back({t.value("source").toString().toStdString(),
                                  t.value("quantity").toString(fe.variable == "precipitation" ? "timeseries"
                                                                                              : "ET_timeseries")
                                      .toStdString()});
        }
        if (fe.targets.empty())
        {
            err = QString("forcing entry '%1': no targets").arg(fe.name.c_str());
            return false;
        }
        cfg.entries.push_back(fe);
    }
    cfg.enabled = !cfg.entries.empty();
    return true;
}

bool fetch(const ForcingConfig &cfg, double t0, double t1, std::vector<ForcingSeries> &out, QString &err)
{
    out.assign(cfg.entries.size(), ForcingSeries());

    // Open-Meteo: one batched request per variable for all distinct points of the cycle.
    std::map<std::string, std::map<QString, HourlyPoint>> om;   // variable -> point key -> hourly data
    for (const char *var : {"precipitation", "et0"})
    {
        std::vector<std::pair<double, double>> pts;
        std::map<QString, int> seen;
        for (const auto &e : cfg.entries)
            if (e.provider == "openmeteo" && e.variable == var)
                for (const auto &p : e.points)
                    if (!seen.count(pointKey(p.lat, p.lon)))
                    {
                        seen[pointKey(p.lat, p.lon)] = 1;
                        pts.push_back({p.lat, p.lon});
                    }
        const size_t batch = 50;
        for (size_t i = 0; i < pts.size(); i += batch)
        {
            std::vector<std::pair<double, double>> b(pts.begin() + i, pts.begin() + std::min(pts.size(), i + batch));
            std::vector<HourlyPoint> res;
            QString e2;
            if (!requestOpenMeteo(b, openMeteoName(var), cfg.pastDays, cfg.forecastDays, res, e2))
            {
                err = e2;
                return false;
            }
            for (size_t k = 0; k < b.size(); ++k) om[var][pointKey(b[k].first, b[k].second)] = res[k];
        }
    }

    QStringList missing;
    for (size_t i = 0; i < cfg.entries.size(); ++i)
    {
        const ForcingEntry &e = cfg.entries[i];
        ForcingSeries &s = out[i];
        const bool pr = e.isPrecipitation();
        if (e.provider == "csv")
        {
            for (const std::string &file : e.files)
            {
                ForcingSeries part;
                QString e2;
                if (!readCsv(file, pr, part, e2)) { err = e2; return false; }
                if (part.value.empty()) continue;
                // the later file wins from its first time on
                const double cut = pr ? part.start.front() : part.t.front();
                size_t keep = 0;
                while (keep < s.value.size() && (pr ? s.end[keep] <= cut + 1e-9 : s.t[keep] < cut - 1e-9)) ++keep;
                s.value.resize(keep);
                if (pr) { s.start.resize(keep); s.end.resize(keep); } else s.t.resize(keep);
                s.value.insert(s.value.end(), part.value.begin(), part.value.end());
                if (pr)
                {
                    s.start.insert(s.start.end(), part.start.begin(), part.start.end());
                    s.end.insert(s.end.end(), part.end.begin(), part.end.end());
                }
                else
                    s.t.insert(s.t.end(), part.t.begin(), part.t.end());
            }
        }
        else
        {
            // weighted combination of the points, hour by hour (time axis of the first point)
            double wsum = 0;
            for (const auto &p : e.points) wsum += p.weight;
            const HourlyPoint &ref = om[e.variable][pointKey(e.points[0].lat, e.points[0].lon)];
            for (size_t k = 0; k < ref.tEnd.size(); ++k)
            {
                double v = 0;
                for (const auto &p : e.points)
                {
                    const HourlyPoint &hp = om[e.variable][pointKey(p.lat, p.lon)];
                    const double mm = (k < hp.mm.size()) ? hp.mm[k] : 0.0;
                    v += p.weight / wsum * mm;
                }
                if (pr)
                {
                    s.start.push_back(ref.tEnd[k] - kHour);
                    s.end.push_back(ref.tEnd[k]);
                    s.value.push_back(v / 1000.0);                  // mm in the hour -> m depth
                }
                else
                {
                    s.t.push_back(ref.tEnd[k] - 0.5 * kHour);
                    s.value.push_back(v / 1000.0 * 24.0);           // mm per hour -> m/day
                }
            }
        }
        for (double &x : s.value) x *= e.scale;
        clip(s, pr, t0 - kHour, t1 + kHour);
        if (!covers(s, pr, t0, t1))
        {
            QString range = "no data";
            if (!s.value.empty())
                range = QString("%1..%2").arg(pr ? s.start.front() : s.t.front(), 0, 'f', 3)
                            .arg(pr ? s.end.back() : s.t.back(), 0, 'f', 3);
            missing << QString::fromStdString(e.name.empty() ? e.targets[0].source : e.name) + " [" + range + "]";
        }
    }
    if (!missing.isEmpty())
    {
        err = QString("forcing does not cover the window %1..%2 for: ").arg(t0, 0, 'f', 3).arg(t1, 0, 'f', 3) + missing.mid(0, 8).join(", ") +
              (missing.size() > 8 ? QString(" (+%1 more)").arg(missing.size() - 8) : QString());
        return false;
    }
    return true;
}
} // namespace DTForcing
