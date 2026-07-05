// PosteriorHistoryLoader.cpp

#include "PosteriorHistoryLoader.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

PosteriorHistoryLoader::PosteriorHistoryLoader(QObject *parent)
    : QObject(parent)
{
}

void PosteriorHistoryLoader::fetch()
{
    if (!m_nam)
    {
        m_nam = new QNetworkAccessManager(this);
        connect(m_nam, &QNetworkAccessManager::finished,
                this, &PosteriorHistoryLoader::onReplyFinished);
    }
    QNetworkRequest req(m_historyUrl);
    // Bust caches; the file is appended to on every cycle.
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    m_nam->get(req);
}

void PosteriorHistoryLoader::onReplyFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError)
    {
        emit failed(reply->errorString());
        return;
    }

    QVector<CycleSummary> out;
    QString err;
    if (!parse(reply->readAll(), out, err))
    {
        emit failed(err);
        return;
    }
    emit loaded(out);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static QVector<double> toVector(const QJsonArray &a)
{
    QVector<double> v;
    v.reserve(a.size());
    for (const QJsonValue &x : a) v.push_back(x.toDouble());
    return v;
}

// ---------------------------------------------------------------------------
bool PosteriorHistoryLoader::parse(const QByteArray &data,
                                   QVector<CycleSummary> &out,
                                   QString &errorMessage) const
{
    out.clear();

    const QList<QByteArray> lines = data.split('\n');
    for (int li = 0; li < lines.size(); ++li)
    {
        const QByteArray line = lines[li].trimmed();
        if (line.isEmpty())
            continue;

        QJsonParseError parseErr;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseErr);
        if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
        {
            // Tolerate a truncated FINAL line: the runner may be
            // mid-append when the fetch lands. Anything earlier is a
            // genuinely corrupt file.
            const bool isLastNonEmpty = [&]() {
                for (int lj = li + 1; lj < lines.size(); ++lj)
                    if (!lines[lj].trimmed().isEmpty()) return false;
                return true;
            }();
            if (isLastNonEmpty)
                break;
            errorMessage = QString("posterior history line %1 is not valid "
                                   "JSON: %2")
                               .arg(li + 1)
                               .arg(parseErr.errorString());
            return false;
        }
        const QJsonObject rec = doc.object();

        CycleSummary cs;
        cs.cycle     = rec.value("cycle").toInt(-1);
        cs.timestamp = QDateTime::fromString(
            rec.value("timestamp").toString(), Qt::ISODate);
        cs.tNow      = rec.value("t_now").toDouble(0.0);

        for (const QJsonValue &v : rec.value("parameter_names").toArray())
            cs.paramNames.push_back(v.toString());

        cs.bestParams = toVector(rec.value("point_estimate").toArray());
        if (cs.paramNames.isEmpty() ||
            cs.bestParams.size() != cs.paramNames.size())
        {
            errorMessage = QString("posterior history line %1: "
                                   "point_estimate/parameter_names size "
                                   "mismatch").arg(li + 1);
            return false;
        }

        // MCMC-mode extras (additive fields on CycleSummary).
        cs.converged         = rec.value("converged").toBool(false);
        cs.ess               = rec.value("ess").toDouble(0.0);
        cs.plateauedFraction = rec.value("plateaued_fraction").toDouble(0.0);
        cs.acceptanceRate    = rec.value("acceptance_rate").toDouble(0.0);
        cs.poolSize          = rec.value("pool_size").toDouble(0.0);

        if (cs.converged &&
            rec.contains("p10") && rec.contains("p90"))
        {
            cs.paramP10 = toVector(rec.value("p10").toArray());
            cs.paramP90 = toVector(rec.value("p90").toArray());
            cs.paramMin = toVector(rec.value("p025").toArray());
            cs.paramMax = toVector(rec.value("p975").toArray());
        }
        else
        {
            // Provisional cycle: no dispersion exists in the record, by
            // design. Zero-width band at the point estimate keeps
            // size-assuming chart code safe without displaying any false
            // spread; renderers that honor cs.converged gap the band
            // entirely (V3).
            cs.paramP10 = cs.bestParams;
            cs.paramP90 = cs.bestParams;
            cs.paramMin = cs.bestParams;
            cs.paramMax = cs.bestParams;
        }

        // Defensive: percentile arrays, when present, must match the
        // parameter count.
        if (cs.paramP10.size() != cs.paramNames.size() ||
            cs.paramP90.size() != cs.paramNames.size())
        {
            errorMessage = QString("posterior history line %1: percentile "
                                   "array size mismatch").arg(li + 1);
            return false;
        }

        // Observation metrics do not exist in MCMC mode:
        // observationNames / usedObsIndices / bestMSE / bestR2 / bestNSE
        // stay empty.

        out.push_back(cs);
    }

    if (out.isEmpty())
    {
        errorMessage = "posterior history contains no parseable records "
                       "(no calibration cycle completed yet?)";
        return false;
    }
    return true;
}
