// PosteriorHistoryLoader.h
//
// Parses posterior_history.jsonl (the per-cycle record appended by
// DTStreamingMCMC::appendHistoryRecord on the runner side) into the same
// QVector<CycleSummary> that GaMergedLoader emits, so AssimViewer's chart
// code is loader-agnostic.
//
// File structure: one compact JSON object per line, e.g.
//
//   {"cycle":3,"timestamp":"2026-07-05T14:22:33Z","t_now":43900.0,
//    "converged":true,"parameter_names":[...],"point_estimate":[...],
//    "mean":[...],"p10":[...],"p90":[...],"p025":[...],"p975":[...],
//    "ess":412.0,"plateaued_fraction":0.94,"acceptance_rate":0.28,
//    "pool_size":9820,"sweeps":1840,"evaluations":29440}
//
// Field mapping onto CycleSummary:
//
//   point_estimate -> bestParams        (MAP / best carried state)
//   p10 / p90      -> paramP10/paramP90 (posterior credible band,
//                                        GA-band-comparable)
//   p025 / p975    -> paramMin/paramMax (95% interval)
//   converged, ess, plateaued_fraction, acceptance_rate, pool_size
//                  -> the MCMC-mode extras on CycleSummary
//
// Provisional records (converged=false) carry NO percentile fields by
// design (transit dispersion is not posterior width). For those cycles
// the band vectors are filled with the point estimate itself — a
// zero-width band — so size-assuming chart code stays safe while no
// false dispersion is ever displayed. The converged flag lets the
// renderer gap the band properly instead (viewer stage V3).
//
// Observation-metric fields (bestMSE/R2/NSE, usedObsIndices) stay empty:
// they do not exist in MCMC mode.
//
// Robustness: blank lines are skipped; an unparseable FINAL line is
// tolerated silently (the runner may be mid-append when the fetch
// lands); an unparseable interior line fails the load.

#pragma once

#include "GaMergedLoader.h"   // CycleSummary

#include <QObject>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

class PosteriorHistoryLoader : public QObject
{
    Q_OBJECT

public:
    explicit PosteriorHistoryLoader(QObject *parent = nullptr);

    void setHistoryUrl(const QUrl &url) { m_historyUrl = url; }

    // Fetch and parse; emits loaded() or failed().
    void fetch();

signals:
    void loaded(const QVector<CycleSummary> &cycles);
    void failed(const QString &errorMessage);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    bool parse(const QByteArray &data,
               QVector<CycleSummary> &out,
               QString &errorMessage) const;

    QUrl                   m_historyUrl;
    QNetworkAccessManager *m_nam = nullptr;
};
