// RealizationCILoader.cpp

#include "RealizationCILoader.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>

#include <limits>

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
    m_pending = 0;

    if (m_urlTemplate.isEmpty() || observationNames.isEmpty())
    {
        emit loaded(m_result);
        return;
    }

    for (const QString &name : observationNames)
    {
        // The runner writes files named by the raw observation name; the
        // URL may need percent-encoding for spaces etc.
        const QString urlStr = m_urlTemplate.arg(QString(QUrl::toPercentEncoding(name)));
        QNetworkRequest req{QUrl(urlStr)};
        req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
        // Stash the observation name on the request so the reply handler
        // knows which band it belongs to.
        req.setAttribute(QNetworkRequest::User, name);
        ++m_pending;
        m_nam->get(req);
    }
}

void RealizationCILoader::onReplyFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    const QString name =
        reply->request().attribute(QNetworkRequest::User).toString();

    if (reply->error() == QNetworkReply::NoError)
    {
        RealizationBand band;
        band.name = name;
        if (parseBracket(reply->readAll(), band) &&
            !band.lo.isEmpty() && !band.hi.isEmpty())
            m_result.insert(name, band);
    }
    // A missing file (404) or parse failure simply yields no band for this
    // observation — expected before the first realization interval.

    if (--m_pending <= 0)
        emit loaded(m_result);
}

// ---------------------------------------------------------------------------
// Parse the repeated-time TimeSeriesSet dump. Header identifies columns;
// we locate the "2.5 %" and "97.5 %" value columns (each preceded by its
// own time column) and read them.
// ---------------------------------------------------------------------------
bool RealizationCILoader::parseBracket(const QByteArray &data,
                                       RealizationBand &out)
{
    const QList<QByteArray> lines = data.split('\n');
    if (lines.isEmpty()) return false;

    // Header: comma-separated names, alternating "t" and a series label.
    const QString header = QString::fromUtf8(lines.first()).trimmed();
    const QStringList cols = header.split(',');

    // Find the value-column index for 2.5 % and 97.5 %. The engine writes
    // labels like "2.500000 %" / "97.500000 %" (std::to_string(pct*100)),
    // so match by prefix rather than exact text.
    int loCol = -1, hiCol = -1;
    for (int i = 0; i < cols.size(); ++i)
    {
        const QString c = cols[i].trimmed();
        if (c.startsWith("2.5"))  loCol = i;   // "2.500000 %"
        if (c.startsWith("97.5")) hiCol = i;   // "97.500000 %"
    }
    if (loCol < 0 || hiCol < 0) return false;

    // Each value column is preceded by its own time column at index-1.
    const int loT = loCol - 1;
    const int hiT = hiCol - 1;
    if (loT < 0 || hiT < 0) return false;

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

        out.lo.push_back(QPointF(static_cast<qreal>(daySerialToMs(t1)), v1));
        out.hi.push_back(QPointF(static_cast<qreal>(daySerialToMs(t2)), v2));
    }
    return true;
}
