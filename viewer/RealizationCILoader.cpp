// RealizationCILoader.cpp

#include "RealizationCILoader.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>

// OHQ day-serial (Excel epoch 1899-12-30) -> ms since Unix epoch.
// Matches CsvLoader's conversion so bands align with the CSV series.
static qint64 daySerialToMs(double serial)
{
    return static_cast<qint64>((serial - 25569.0) * 86400000.0);
}

RealizationCILoader::RealizationCILoader(QObject *parent)
    : QObject(parent)
{
}

void RealizationCILoader::fetch(const QStringList &observationNames)
{
    if (!m_nam)
    {
        m_nam = new QNetworkAccessManager(this);
        connect(m_nam, &QNetworkAccessManager::finished,
                this, &RealizationCILoader::onReplyFinished);
    }

    m_result.clear();
    m_names = observationNames;

    if (m_url.isEmpty() || observationNames.isEmpty())
    {
        emit loaded(m_result);
        return;
    }

    // One GET for the whole combined file.
    QNetworkRequest req{QUrl(m_url)};
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    m_nam->get(req);
}

void RealizationCILoader::onReplyFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() == QNetworkReply::NoError)
        m_result = parseCombined(reply->readAll(), m_names);
    // A missing file (404) or parse failure simply yields no bands -- expected
    // before the first realization interval has produced the file.

    emit loaded(m_result);
}

// ---------------------------------------------------------------------------
// Parse the combined repeated-time TimeSeriesSet dump. The header alternates
// "t" columns with value columns labeled "<obs> | <pct> %". For each
// requested observation name we locate its "2.5 %" and "97.5 %" value columns
// (each preceded by its own time column) and read the lo/hi band.
//
// The " | " separator between the observation name and the percentile label
// prevents a name that is a prefix of another (e.g. "Soil" vs "Soil Moisture")
// from matching the wrong column.
// ---------------------------------------------------------------------------
QHash<QString, RealizationBand>
RealizationCILoader::parseCombined(const QByteArray &data,
                                   const QStringList &names)
{
    QHash<QString, RealizationBand> out;

    const QList<QByteArray> lines = data.split('\n');
    if (lines.isEmpty()) return out;

    const QString header = QString::fromUtf8(lines.first()).trimmed();
    const QStringList cols = header.split(',');

    for (const QString &name : names)
    {
        // Value-column labels the engine writes: "<name> | 2.500000 %" and
        // "<name> | 97.500000 %" (std::to_string(pct*100)); match by prefix.
        const QString loPrefix = name + " | 2.5";
        const QString hiPrefix = name + " | 97.5";

        int loCol = -1, hiCol = -1;
        for (int i = 0; i < cols.size(); ++i)
        {
            const QString c = cols[i].trimmed();
            if (c.startsWith(loPrefix)) loCol = i;
            if (c.startsWith(hiPrefix)) hiCol = i;
        }
        if (loCol < 0 || hiCol < 0) continue;   // this obs not in the file

        // Each value column is preceded by its own time column at index-1.
        const int loT = loCol - 1;
        const int hiT = hiCol - 1;
        if (loT < 0 || hiT < 0) continue;

        RealizationBand band;
        band.name = name;
        for (int li = 1; li < lines.size(); ++li)
        {
            const QByteArray raw = lines[li].trimmed();
            if (raw.isEmpty()) continue;
            const QList<QByteArray> f = raw.split(',');
            if (f.size() <= hiCol) continue;

            bool okT1, okV1, okT2, okV2;
            const double t1 = QString(f[loT]).toDouble(&okT1);
            const double v1 = QString(f[loCol]).toDouble(&okV1);
            const double t2 = QString(f[hiT]).toDouble(&okT2);
            const double v2 = QString(f[hiCol]).toDouble(&okV2);
            if (!(okT1 && okV1 && okT2 && okV2)) continue;

            band.lo.push_back(QPointF(static_cast<qreal>(daySerialToMs(t1)), v1));
            band.hi.push_back(QPointF(static_cast<qreal>(daySerialToMs(t2)), v2));
        }

        if (!band.lo.isEmpty() && !band.hi.isEmpty())
            out.insert(name, band);
    }

    return out;
}
