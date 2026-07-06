// RealizationCILoader.h
//
// Fetches the SINGLE combined posterior-predictive band file written by
// DTStreamingMCMC::produceRealizationCI:
//
//     realization_ci_latest.txt
//
// One TimeSeriesSet::write() dump holds, for every calibration observation,
// the {Mean, 2.5 %, 50 %, 97.5 %} series in the engine's REPEATED-TIME
// layout, with each value column qualified by the observation name:
//
//     t, <obs> | Mean, t, <obs> | 2.5 %, t, <obs> | 50 %, t, <obs> | 97.5 %, t, <obs2> | Mean, ...
//     43833.00000,0.12,43833.00000,0.02,43833.00000,0.11,43833.00000,0.31,...
//     ...
//
// i.e. every series carries its own time column. For each requested
// observation name the loader locates that observation's "2.5 %" and
// "97.5 %" value columns (each paired with its preceding t column) and
// returns them as a lo/hi QPointF band, with x in milliseconds since the
// Unix epoch (OHQ day-serial converted the same way CsvLoader does), ready
// to feed a QAreaSeries on the matching Comparison panel.
//
// The observation name lives in the column LABEL, not the filename, so names
// containing '/' (e.g. "Underdrain flow (m3/day)") round-trip correctly --
// the old per-observation filename scheme silently dropped them.
//
// One GET per refresh: the loader is handed the ordered list of observation
// names (the comparison-panel names) and the file URL, fetches the one file,
// and emits loaded() with a band per name that was found in it. A missing
// file (404) before the first realization interval simply yields no bands.

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

    // Direct URL of the combined band file, e.g.
    //   http://host/outputs/realization_ci_latest.txt
    void setUrl(const QString &url) { m_url = url; }

    // Fetch the combined file and extract bands for these observation names.
    // Emits loaded() when the request completes (a missing file is not a
    // failure -- it yields an empty result).
    void fetch(const QStringList &observationNames);

signals:
    // Keyed by observation name; only observations whose columns were found
    // and parsed are present.
    void loaded(const QHash<QString, RealizationBand> &bands);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    // Parse the combined repeated-time dump, pulling one band per requested
    // observation name.
    static QHash<QString, RealizationBand> parseCombined(
        const QByteArray &data, const QStringList &names);

    QString                m_url;
    QNetworkAccessManager *m_nam = nullptr;
    QStringList            m_names;   // names requested by the in-flight fetch
    QHash<QString, RealizationBand> m_result;
};
