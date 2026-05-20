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

#include "TimeSeriesSet.h"

#include <QDateTime>
#include <QString>

#include <map>
#include <string>

// ---------------------------------------------------------------------------
// DTObservationBuffer
//
// In-memory mirror of the observation source's current contents.
//
// Source model: HTTP. The buffer fetches a CSV (mandatory) and a sidecar
// JSON (optional) from configured URLs. On each refresh() the buffer's
// contents are *replaced* with whatever the source currently serves —
// this matches the Truth Twin's CSV semantics, which can rewrite trailing
// rows when its forecast tail is updated. (For real sensor sources that
// only ever append, a future DTObservationSource subclass can implement
// incremental append-only ingest.)
//
// The buffer is pure data + I/O. It owns no timer; the policy of *when*
// to refresh lives in DTAssimilation. The buffer can be tested in
// isolation against a static fixture URL or a local file:// URL.
//
// Threading: not thread-safe. All methods must be called from the same
// thread (in practice, the Qt main event-loop thread).
// ---------------------------------------------------------------------------
class DTObservationBuffer
{
public:
    DTObservationBuffer() = default;

    // Configure the source endpoints. csvUrl is mandatory; metaUrl is
    // optional (empty disables sigma extraction).
    void setEndpoints(const QString &csvUrl, const QString &metaUrl = QString());

    // Synchronously fetch CSV (and meta, if configured), parse, and
    // replace internal state. Uses a local QEventLoop to wait for the
    // network reply.
    //
    // Returns true on a successful CSV fetch+parse. Meta-fetch failure is
    // non-fatal (sigma stays at 0). On CSV failure the buffer keeps its
    // previous contents and logs the error.
    bool refresh();

    // Read-only access to the current observations. Variable-name keyed,
    // each variable a TimeSeries<double> sorted by time.
    const TimeSeriesSet<double> &observations() const { return m_obs; }

    // Convenience: fetch a single variable's series, or empty if absent.
    TimeSeries<double> series(const std::string &name) const;

    // Observation noise standard deviation as reported by the meta sidecar.
    // Supports legacy scalar noise_sigma and object-style noise_sigma maps.
    // sigma() returns the default/fallback sigma.
    // 0 if no meta URL configured, or if the sidecar didn't provide one.
    double sigma() const { return m_sigma; }

    // Optional object-style per-observation noise map parsed from the sidecar.
    // Keys are normalized to lower-case, matching the config/runtime convention.
    const std::map<std::string, double> &sigmaByPattern() const
    {
        return m_sigmaByPattern;
    }

    // Diagnostics.
    bool      empty()           const;
    size_t    variableCount()   const;
    size_t    pointCount()      const;
    double    tMin()            const;
    double    tMax()            const;
    QDateTime lastRefreshUtc()  const { return m_lastRefresh; }
    QString   lastError()       const { return m_lastError; }

    QString csvUrl()  const { return m_csvUrl;  }
    QString metaUrl() const { return m_metaUrl; }

private:
    // Helpers (sync HTTP via QNetworkAccessManager + QEventLoop).
    bool fetchToBytes(const QString &url, QByteArray &outBytes,
                      QString &outErr) const;

    // Parse a CSV byte buffer into a TimeSeriesSet<double> following the
    // OHQ "selected_output.csv" layout, where column pairs (t_i, value_i)
    // alternate with each variable having its own time column.
    bool parseCsv(const QByteArray &bytes, TimeSeriesSet<double> &out,
                  QString &outErr) const;

    // Parse the sidecar JSON to extract observation-noise sigma information.
    // Supports both:
    //   { "noise_sigma": <double> }
    //   { "observations": { "noise_sigma": <double-or-object> } }
    // For object-style noise_sigma, outSigma receives the "default" entry
    // when present, otherwise 0; outSigmaByPattern receives all non-default
    // entries with lower-case keys.
    bool parseMeta(const QByteArray &bytes,
                   double &outSigma,
                   std::map<std::string, double> &outSigmaByPattern,
                   QString &outErr) const;

    QString m_csvUrl;
    QString m_metaUrl;

    TimeSeriesSet<double>       m_obs;
    double                      m_sigma = 0.0;
    std::map<std::string,double> m_sigmaByPattern;
    QDateTime                   m_lastRefresh;
    QString               m_lastError;
};
