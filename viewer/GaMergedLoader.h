// GaMergedLoader.h
//
// Parses ga_output_merged.txt (the per-cycle archive written by
// DTAssimilation::archiveGAOutput) into a structured list of cycle
// summaries that the AssimViewer consumes.
//
// File structure (one block per calibration cycle):
//
//   === Cycle 0 | timestamp 2026-05-18 15:52:16 | t_now=43837.000000 ===
//   Generation: 0
//   ID, <param1>, <param2>, ..., likelihood, Fitness, Rank, <obs1>_MSE, <obs1>_R2, <obs1>_NSE, <obs2>_MSE, ...
//   0, <values...>
//   1, <values...>
//   ...
//   Generation: 1
//   ...
//   <last generation of cycle>
//   === Cycle 1 | ... ===
//   ...
//
// Each cycle's "best individual" is identified by Rank == 1 within the
// LAST generation of the cycle. Population statistics (min, max, p10,
// p90 per parameter) are computed across all individuals of that
// terminal generation.
//
// Observations whose MSE, R2 and NSE are all 0 for the best individual
// are assumed to have no observation data in the buffer and are
// filtered out of "used" observations.

#pragma once

#include <QObject>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

// One row in the per-cycle summary.
struct CycleSummary
{
    int        cycle    = -1;
    QDateTime  timestamp;             // wall-clock from the cycle header
    double     tNow     = 0.0;        // OHQ simulated day-serial

    // Parameter names, learned from the column header. Same length and
    // order across all cycles within a single file.
    QStringList paramNames;

    // Observation names, in column order. Includes ALL observations
    // present in the model, including the ones with no data.
    QStringList observationNames;

    // Indices into observationNames that have at least one non-zero
    // metric for the best individual (i.e., observations that
    // actually contributed to the misfit).
    QVector<int> usedObsIndices;

    // Best-individual values (Rank == 1 in the terminal generation).
    QVector<double> bestParams;       // size == paramNames.size()
    QVector<double> bestMSE;          // size == observationNames.size()
    QVector<double> bestR2;
    QVector<double> bestNSE;

    // Population stats per parameter, computed across all individuals
    // of the terminal generation (size == paramNames.size()).
    QVector<double> paramMin;
    QVector<double> paramMax;
    QVector<double> paramP10;
    QVector<double> paramP90;

    // --- MCMC-mode extras (PosteriorHistoryLoader) ---------------------
    // Defaults chosen so GA-mode records read as "converged, no sampler
    // diagnostics"; GaMergedLoader never touches these.
    bool   converged         = true;
    double ess               = 0.0;
    double plateauedFraction = 0.0;
    double acceptanceRate    = 0.0;
    double poolSize          = 0.0;
};

class GaMergedLoader : public QObject
{
    Q_OBJECT

public:
    explicit GaMergedLoader(QObject *parent = nullptr);

    void setMergedUrl(const QUrl &url) { m_mergedUrl = url; }

    // Trigger an asynchronous fetch + parse. On success, emits loaded();
    // on failure, emits failed().
    void fetch();

signals:
    void loaded(const QVector<CycleSummary> &cycles);
    void failed(const QString &errorMessage);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    // Parse the entire file content into cycles.
    bool parse(const QString &text,
               QVector<CycleSummary> &out,
               QString &err);

    // Helpers.
    static bool parseCycleHeader(const QString &line,
                                 int &cycle,
                                 QDateTime &timestamp,
                                 double &tNow);

    // Parses one ", "-separated column header for a Generation block.
    // Returns the count of parameter columns, the observation names,
    // and the indices of the likelihood / Fitness / Rank / first
    // _MSE columns in the data rows.
    static bool parseColumnHeader(const QString &line,
                                  QStringList &paramNames,
                                  QStringList &observationNames,
                                  int &idxLikelihood,
                                  int &idxFitness,
                                  int &idxRank,
                                  int &idxFirstMetric);

    QNetworkAccessManager *m_nam = nullptr;
    QUrl                   m_mergedUrl;
};
