// PosteriorDistLoader.h
//
// Fetches the latest per-parameter posterior histograms written by
// DTStreamingMCMC::writePosteriorDistribution:
//
//     posterior_dist_latest.txt
//
// A TimeSeriesSet::write() dump in the engine's REPEATED-TIME layout, one
// (t, value) column pair per calibrated parameter, where for each parameter
// the "t" column holds the histogram BIN CENTER and the value column holds
// the bin FREQUENCY (count) -- i.e. TimeSeries::distribution() output:
//
//     t, EngineeredSoilAlpha, t, EngineeredSoilKsat, ...
//     0.27737, 1.6014, 6.47784, 1.11002, ...
//     ...
//
// For each parameter the loader returns the histogram as a QVector<QPointF>
// with x = bin center, y = frequency, keyed by parameter name -- ready to
// feed a filled area/line series on the Posterior tab.
//
// One GET per refresh. A missing file (before the first realization interval)
// yields an empty result, which is not an error.

#pragma once

#include <QObject>
#include <QHash>
#include <QPointF>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

class PosteriorDistLoader : public QObject
{
    Q_OBJECT

public:
    explicit PosteriorDistLoader(QObject *parent = nullptr);

    // Direct URL of the histogram file, e.g.
    //   http://host/outputs/posterior_dist_latest.txt
    void setUrl(const QString &url) { m_url = url; }

    void fetch();

signals:
    // Keyed by parameter name; x = bin center, y = frequency.
    void loaded(const QHash<QString, QVector<QPointF>> &hists);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    static QHash<QString, QVector<QPointF>> parse(const QByteArray &data);

    QString                m_url;
    QNetworkAccessManager *m_nam = nullptr;
};
