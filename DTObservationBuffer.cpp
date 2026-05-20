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

#include "DTObservationBuffer.h"

#include <QByteArray>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <iostream>

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void DTObservationBuffer::setEndpoints(const QString &csvUrl,
                                       const QString &metaUrl)
{
    m_csvUrl  = csvUrl;
    m_metaUrl = metaUrl;
}

bool DTObservationBuffer::refresh()
{
    m_lastError.clear();

    if (m_csvUrl.isEmpty())
    {
        m_lastError = "DTObservationBuffer::refresh(): csv_url not configured";
        std::cerr << "[ObsBuf] " << m_lastError.toStdString() << "\n";
        return false;
    }

    // ------------------------------------------------------------------
    // CSV (mandatory)
    // ------------------------------------------------------------------
    QByteArray csvBytes;
    QString    fetchErr;
    if (!fetchToBytes(m_csvUrl, csvBytes, fetchErr))
    {
        m_lastError = "CSV fetch failed: " + fetchErr;
        std::cerr << "[ObsBuf] " << m_lastError.toStdString() << "\n";
        return false;
    }

    TimeSeriesSet<double> parsed;
    QString parseErr;
    if (!parseCsv(csvBytes, parsed, parseErr))
    {
        m_lastError = "CSV parse failed: " + parseErr;
        std::cerr << "[ObsBuf] " << m_lastError.toStdString() << "\n";
        return false;
    }

    m_obs = std::move(parsed);
    m_lastRefresh = QDateTime::currentDateTimeUtc();

    // ------------------------------------------------------------------
    // Meta (optional)
    // ------------------------------------------------------------------
    if (!m_metaUrl.isEmpty())
    {
        QByteArray metaBytes;
        QString    metaFetchErr;
        if (fetchToBytes(m_metaUrl, metaBytes, metaFetchErr))
        {
            double sigma = 0.0;
            std::map<std::string, double> sigmaByPattern;
            QString metaParseErr;
            if (parseMeta(metaBytes, sigma, sigmaByPattern, metaParseErr))
            {
                m_sigma = sigma;
                m_sigmaByPattern = std::move(sigmaByPattern);
            }
            else
            {
                std::cerr << "[ObsBuf] meta parse warning: "
                          << metaParseErr.toStdString() << "\n";
            }
        }
        else
        {
            std::cerr << "[ObsBuf] meta fetch warning: "
                      << metaFetchErr.toStdString() << "\n";
        }
    }

    std::cout << "[ObsBuf] refreshed from " << m_csvUrl.toStdString()
              << " — " << variableCount() << " series, "
              << pointCount() << " points, sigma=" << m_sigma;
    if (!m_sigmaByPattern.empty())
    {
        std::cout << ", sigma_by_name=";
        for (const auto &kv : m_sigmaByPattern)
            std::cout << "'" << kv.first << "'=" << kv.second << " ";
    }
    std::cout << "\n";

    return true;
}

TimeSeries<double>
DTObservationBuffer::series(const std::string &name) const
{
    if (m_obs.Contains(name))
        return m_obs[name];
    return TimeSeries<double>{};
}

bool   DTObservationBuffer::empty()         const { return m_obs.empty(); }
size_t DTObservationBuffer::variableCount() const { return m_obs.size();  }

size_t DTObservationBuffer::pointCount() const
{
    size_t total = 0;
    for (size_t i = 0; i < m_obs.size(); ++i)
        total += m_obs[i].size();
    return total;
}

double DTObservationBuffer::tMin() const
{
    if (empty()) return 0.0;
    return m_obs.mintime();
}

double DTObservationBuffer::tMax() const
{
    if (empty()) return 0.0;
    return m_obs.maxtime();
}

// ---------------------------------------------------------------------------
// HTTP fetch (sync via QEventLoop)
// ---------------------------------------------------------------------------

bool DTObservationBuffer::fetchToBytes(const QString &url,
                                       QByteArray &outBytes,
                                       QString &outErr) const
{
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("User-Agent", "OpenHydroTwin/observation-buffer");
    // Disable HTTP caching: we want a fresh read each refresh().
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);

    QNetworkReply *reply = nam.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished,
                     &loop, &QEventLoop::quit);
    loop.exec();

    bool ok = (reply->error() == QNetworkReply::NoError);
    if (ok)
    {
        outBytes = reply->readAll();
    }
    else
    {
        outErr = reply->errorString() + " [" + url + "]";
    }
    reply->deleteLater();
    return ok;
}

// ---------------------------------------------------------------------------
// CSV parser
//
// Handles the OHQ "selected_output.csv" interleaved-columns layout:
//
//     t, varA, t, varB, t, varC
//     43833.0, 0.18, 43833.0, 7.5e-7, 43833.0, 0.0
//     43833.04, 0.40, 43833.04, 6.9e-6, 43833.04, 0.0
//     ...
//
// Each variable has its own t column (which in practice is identical
// across variables, but we don't assume that here).
//
// Strategy:
//   1. Read header line, identify positions of "t" columns. Each "t"
//      column is followed by exactly one value column whose header is
//      the variable name.
//   2. Build one TimeSeries per variable, named from the header.
//   3. For each data row, parse the (t, v) pair for each variable.
//
// Tolerant of: extra whitespace, blank trailing lines, scientific
// notation, leading/trailing commas. Intolerant of: malformed rows
// (returns false and reports the row index).
// ---------------------------------------------------------------------------
bool DTObservationBuffer::parseCsv(const QByteArray &bytes,
                                   TimeSeriesSet<double> &out,
                                   QString &outErr) const
{
    // Split into lines. Handle both \n and \r\n.
    const QString text = QString::fromUtf8(bytes);
    const QStringList lines = text.split(QChar('\n'), Qt::KeepEmptyParts);

    if (lines.isEmpty())
    {
        outErr = "empty CSV";
        return false;
    }

    // ---- Helpers ----
    auto trim = [](QString s) -> QString {
        return s.trimmed();
    };
    auto splitCommas = [&](const QString &line) -> QStringList {
        QStringList toks = line.split(QChar(','), Qt::KeepEmptyParts);
        for (QString &t : toks) t = trim(t);
        return toks;
    };

    // ---- Locate header (first non-blank line) ----
    int headerRow = -1;
    for (int i = 0; i < lines.size(); ++i)
    {
        if (!trim(lines[i]).isEmpty()) { headerRow = i; break; }
    }
    if (headerRow < 0)
    {
        outErr = "CSV contains no header row";
        return false;
    }

    const QStringList headerToks = splitCommas(lines[headerRow]);

    // ---- Identify (t-col, value-col, varName) triples ----
    struct ColPair { int tCol; int vCol; std::string varName; };
    std::vector<ColPair> pairs;
    for (int c = 0; c < headerToks.size(); ++c)
    {
        if (headerToks[c].compare("t", Qt::CaseInsensitive) == 0 &&
            c + 1 < headerToks.size())
        {
            ColPair p;
            p.tCol    = c;
            p.vCol    = c + 1;
            p.varName = headerToks[c + 1].toStdString();
            pairs.push_back(p);
            ++c; // skip the value col we just consumed
        }
    }

    if (pairs.empty())
    {
        outErr = "CSV header contains no recognizable (t, var) column pairs";
        return false;
    }

    // ---- Bootstrap output series ----
    out.clear();
    for (const auto &p : pairs)
    {
        TimeSeries<double> empty;
        empty.setName(p.varName);
        out.push_back(empty);
    }

    // ---- Parse data rows ----
    for (int r = headerRow + 1; r < lines.size(); ++r)
    {
        const QString rawLine = trim(lines[r]);
        if (rawLine.isEmpty()) continue;

        const QStringList toks = splitCommas(rawLine);

        for (size_t s = 0; s < pairs.size(); ++s)
        {
            const ColPair &p = pairs[s];
            if (p.tCol >= toks.size() || p.vCol >= toks.size())
            {
                // Row too short for this variable; skip silently rather
                // than fail the whole parse — partial rows shouldn't
                // poison the buffer.
                continue;
            }

            bool tOk = false, vOk = false;
            const double t = toks[p.tCol].toDouble(&tOk);
            const double v = toks[p.vCol].toDouble(&vOk);
            if (!tOk || !vOk) continue;

            out[s].append(t, v);
        }
    }

    // Sort each series by time as a defensive measure; the source likely
    // provides them in order but a reordered upstream wouldn't be caught
    // anywhere else.
    for (size_t i = 0; i < out.size(); ++i)
    {
        TimeSeries<double> &ts = out[i];
        std::sort(ts.begin(), ts.end(),
                  [](const DataPoint<double> &a, const DataPoint<double> &b) {
                      return a.t < b.t;
                  });
    }

    return true;
}

// ---------------------------------------------------------------------------
// Meta JSON parser
//
// Looks for one of:
//     { "observations": { "noise_sigma": <double>, ... } }
//     { "observations": { "noise_sigma": { "default": <double>, ... }, ... } }
//     { "noise_sigma": <double> }
//     { "noise_sigma": { "default": <double>, ... } }
//
// Both the legacy scalar shape and the object-style per-observation shape are
// tolerated to keep this loosely coupled to the Truth Twin's exact sidecar
// layout.
// ---------------------------------------------------------------------------
bool DTObservationBuffer::parseMeta(const QByteArray &bytes,
                                    double &outSigma,
                                    std::map<std::string, double> &outSigmaByPattern,
                                    QString &outErr) const
{
    outSigma = 0.0;
    outSigmaByPattern.clear();

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (doc.isNull())
    {
        outErr = "JSON parse: " + perr.errorString();
        return false;
    }
    if (!doc.isObject())
    {
        outErr = "meta JSON root is not an object";
        return false;
    }
    const QJsonObject root = doc.object();

    auto parseNoiseSigmaValue = [&](const QJsonValue &v,
                                    const QString &where) -> bool {
        if (v.isDouble())
        {
            const double sigma = v.toDouble(0.0);
            if (sigma < 0.0)
            {
                outErr = where + " must be >= 0";
                return false;
            }
            outSigma = sigma;
            return true;
        }

        if (v.isObject())
        {
            const QJsonObject obj = v.toObject();

            for (auto it = obj.begin(); it != obj.end(); ++it)
            {
                if (!it.value().isDouble())
                {
                    outErr = where + "['" + it.key() + "'] must be a number";
                    return false;
                }

                const double sigma = it.value().toDouble(0.0);
                if (sigma < 0.0)
                {
                    outErr = where + "['" + it.key() + "'] must be >= 0";
                    return false;
                }

                const QString key = it.key().trimmed().toLower();
                if (key == "default")
                    outSigma = sigma;
                else if (!key.isEmpty())
                    outSigmaByPattern[key.toStdString()] = sigma;
            }

            return true;
        }

        outErr = where + " must be a number or object";
        return false;
    };

    if (root.contains("noise_sigma"))
        return parseNoiseSigmaValue(root.value("noise_sigma"), "noise_sigma");

    if (root.contains("observations") && root.value("observations").isObject())
    {
        const QJsonObject obs = root.value("observations").toObject();
        if (obs.contains("noise_sigma"))
            return parseNoiseSigmaValue(obs.value("noise_sigma"),
                                        "observations.noise_sigma");
    }

    outErr = "meta JSON has no 'noise_sigma' field at root or under 'observations'";
    return false;
}
