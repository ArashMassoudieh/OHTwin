// RealizationCILoader.h
//
// Fetches the per-observation posterior-predictive 95% bracket files
// written by DTStreamingMCMC::produceRealizationCI:
//
//     realization_ci_<obs>_latest.txt
//
// Each file is a TimeSeriesSet::write() dump of {Mean, 2.5 %, 50 %,
// 97.5 %} series in the engine's REPEATED-TIME layout:
//
//     t, Mean, t, 2.5 %, t, 50 %, t, 97.5 %
//     43833.00000,0.12,43833.00000,0.02,43833.00000,0.11,43833.00000,0.31
//     ...
//
// i.e. every series carries its own time column. The loader extracts the
// "2.5 %" and "97.5 %" value columns (each paired with its preceding t
// column) and returns them as a lo/hi QPointF band per observation, with
// x in milliseconds since the Unix epoch (OHQ day-serial converted the
// same way CsvLoader does), ready to feed a QAreaSeries on the matching
// Comparison panel.
//
// One loader fetches ALL observations: it is given the ordered list of
// observation names (from the modeled/observed CSVs) and a URL template,
// fires one GET per name, and emits loaded() once all have returned
// (successes and misses both count as "returned"; a missing file for an
// observation simply yields no band for it, which is normal before the
// first realization interval).

#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// One observation's predictive band.
struct RealizationBand
{
    QString        name;
    QList<QPointF> lo;    // 2.5 %  (x = ms epoch, y = value)
    QList<QPointF> hi;    // 97.5 %
};

class RealizationCILoader : public QObject
{
    Q_OBJECT

public:
    explicit RealizationCILoader(QObject *parent = nullptr);

    // urlTemplate must contain "%1" where the observation name goes, e.g.
    //   http://host/outputs/realization_ci_%1_latest.txt
    void setUrlTemplate(const QString &urlTemplate) { m_urlTemplate = urlTemplate; }

    // Fetch bands for these observation names. Emits loaded() when all
    // requests have completed (missing files are skipped, not failed).
    void fetch(const QStringList &observationNames);

signals:
    // Keyed by observation name; only observations with a parseable file
    // are present.
    void loaded(const QHash<QString, RealizationBand> &bands);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    static bool parseBracket(const QByteArray &data, RealizationBand &out);

    QString                m_urlTemplate;
    QNetworkAccessManager *m_nam = nullptr;
    int                    m_pending = 0;
    QHash<QString, RealizationBand> m_result;
};
