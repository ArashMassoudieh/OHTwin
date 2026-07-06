// PosteriorDistLoader.cpp

#include "PosteriorDistLoader.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>

PosteriorDistLoader::PosteriorDistLoader(QObject *parent)
    : QObject(parent)
{
}

void PosteriorDistLoader::fetch()
{
    if (!m_nam)
    {
        m_nam = new QNetworkAccessManager(this);
        connect(m_nam, &QNetworkAccessManager::finished,
                this, &PosteriorDistLoader::onReplyFinished);
    }

    if (m_url.isEmpty())
    {
        emit loaded({});
        return;
    }

    QNetworkRequest req{QUrl(m_url)};
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    m_nam->get(req);
}

void PosteriorDistLoader::onReplyFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    QHash<QString, QVector<QPointF>> hists;
    if (reply->error() == QNetworkReply::NoError)
        hists = parse(reply->readAll());
    // A missing file (404) or parse failure simply yields no histograms.

    emit loaded(hists);
}

// ---------------------------------------------------------------------------
// Parse the repeated-time histogram dump. The header alternates "t" columns
// with parameter-name columns; for each parameter the "t" column (index-1)
// holds bin centers and the value column holds frequencies.
// ---------------------------------------------------------------------------
QHash<QString, QVector<QPointF>>
PosteriorDistLoader::parse(const QByteArray &data)
{
    QHash<QString, QVector<QPointF>> out;

    const QList<QByteArray> lines = data.split('\n');
    if (lines.isEmpty()) return out;

    const QString header = QString::fromUtf8(lines.first()).trimmed();
    const QStringList cols = header.split(',');

    // Collect (paramName, valueColumnIndex). Value columns are the ones whose
    // label is not "t"; their bin-center column is at index-1.
    QVector<QPair<QString, int>> params;
    for (int i = 0; i < cols.size(); ++i)
    {
        const QString c = cols[i].trimmed();
        if (c.isEmpty() || c == "t") continue;
        if (i == 0) continue;   // a value column must have a preceding t column
        params.push_back({c, i});
    }
    if (params.isEmpty()) return out;

    for (const auto &p : params)
    {
        const int valCol = p.second;
        const int tCol   = valCol - 1;
        QVector<QPointF> hist;

        for (int li = 1; li < lines.size(); ++li)
        {
            const QByteArray raw = lines[li].trimmed();
            if (raw.isEmpty()) continue;
            const QList<QByteArray> f = raw.split(',');
            if (f.size() <= valCol) continue;

            bool okX, okY;
            const double x = QString(f[tCol]).toDouble(&okX);   // bin center
            const double y = QString(f[valCol]).toDouble(&okY); // frequency
            if (!(okX && okY)) continue;
            hist.push_back(QPointF(x, y));
        }

        if (!hist.isEmpty())
            out.insert(p.first, hist);
    }

    return out;
}
