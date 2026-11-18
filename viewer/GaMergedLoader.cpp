// GaMergedLoader.cpp

#include "GaMergedLoader.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cmath>

GaMergedLoader::GaMergedLoader(QObject *parent)
    : QObject(parent)
{
}

void GaMergedLoader::fetch()
{
    if (!m_nam)
    {
        m_nam = new QNetworkAccessManager(this);
        connect(m_nam, &QNetworkAccessManager::finished,
                this, &GaMergedLoader::onReplyFinished);
    }
    QNetworkRequest req(m_mergedUrl);
    // Bust caches; the file is appended to on every cycle.
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    m_nam->get(req);
}

void GaMergedLoader::onReplyFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError)
    {
        emit failed(reply->errorString());
        return;
    }

    const QString text = QString::fromUtf8(reply->readAll());
    QVector<CycleSummary> out;
    QString err;
    if (!parse(text, out, err))
    {
        emit failed(err);
        return;
    }
    emit loaded(out);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Trim whitespace, then split by ',', then trim each token. Empty tokens
// are preserved so callers can detect the stray empty field that GA.hpp
// emits between "Rank" and the first "_MSE" in data rows.
static QStringList splitFields(const QString &line)
{
    const QStringList parts = line.split(',', Qt::KeepEmptyParts);
    QStringList out;
    out.reserve(parts.size());
    for (const QString &p : parts) out.push_back(p.trimmed());
    return out;
}

bool GaMergedLoader::parseCycleHeader(const QString &line,
                                      int &cycle,
                                      QDateTime &timestamp,
                                      double &tNow)
{
    // === Cycle 0 | timestamp 2026-05-18 15:52:16 | t_now=43837.000000 ===
    static const QRegularExpression re(
        R"(^=== *Cycle *(\d+) *\| *timestamp *([0-9\-: ]+) *\| *t_now=([0-9eE+\-.]+) *===\s*$)");
    const auto m = re.match(line);
    if (!m.hasMatch()) return false;

    cycle = m.captured(1).toInt();
    timestamp = QDateTime::fromString(m.captured(2).trimmed(),
                                      "yyyy-MM-dd HH:mm:ss");
    timestamp.setTimeSpec(Qt::UTC);
    tNow = m.captured(3).toDouble();
    return true;
}

bool GaMergedLoader::parseColumnHeader(const QString &line,
                                       QStringList &paramNames,
                                       QStringList &observationNames,
                                       int &idxLikelihood,
                                       int &idxFitness,
                                       int &idxRank,
                                       int &idxFirstMetric)
{
    // Header shape:
    //   ID, p1, p2, ..., likelihood, Fitness, Rank, obs1_MSE, obs1_R2, obs1_NSE, obs2_MSE, ...
    //
    // The header itself has no stray empty fields (those only appear in
    // data rows). We split on ',' and walk through.

    QStringList tok = splitFields(line);
    while (!tok.isEmpty() && tok.last().isEmpty())
        tok.removeLast();          // drop trailing empty from a final ","

    if (tok.size() < 5 || tok.front() != "ID")
        return false;

    // Locate the fixed-name columns.
    // GA logs renamed this objective in September 2026. Both formats
    // have the same column layout; accept current and historical archives.
    idxLikelihood = tok.indexOf("neg_log_likelihood");
    if (idxLikelihood < 0)
        idxLikelihood = tok.indexOf("likelihood");
    idxFitness    = tok.indexOf("Fitness");
    idxRank       = tok.indexOf("Rank");
    if (idxLikelihood < 0 || idxFitness < 0 || idxRank < 0)
        return false;

    // Parameter names: everything between "ID" and "likelihood".
    paramNames.clear();
    for (int i = 1; i < idxLikelihood; ++i)
        paramNames.push_back(tok.at(i));

    // Observation columns start right after "Rank" and appear in
    // groups of three: <obs>_MSE, <obs>_R2, <obs>_NSE. Strip "_MSE" to
    // get the observation name; verify the next two columns match the
    // expected suffixes.
    observationNames.clear();
    idxFirstMetric = idxRank + 1;
    int i = idxFirstMetric;
    while (i + 2 < tok.size())
    {
        const QString &mseCol = tok.at(i);
        const QString &r2Col  = tok.at(i + 1);
        const QString &nseCol = tok.at(i + 2);

        if (!mseCol.endsWith("_MSE") ||
            !r2Col .endsWith("_R2")  ||
            !nseCol.endsWith("_NSE"))
        {
            // Defensive: bail out cleanly if the structure ever drifts.
            return false;
        }

        QString name = mseCol;
        name.chop(4);   // drop "_MSE"
        observationNames.push_back(name);
        i += 3;
    }
    return true;
}

// Parse one data row and extract:
//   - the parameter values (size == nParams)
//   - the rank
//   - per-observation MSE / R2 / NSE (size == nObs each)
//
// The data row has the SAME structure as the header EXCEPT for one
// quirk in GA.hpp: line 651 prints ",%le, %le, %le" for each obs
// triple, but the preceding "%i, " for Rank already emitted a comma.
// So a stray empty field appears between Rank and the first MSE.
//
// We tolerate that by skipping any empty tokens between idxRank and
// idxFirstMetric. If there are no empties, we still match correctly
// against the header's column positions.
static bool parseDataRow(const QString &line,
                         int nParams,
                         int idxLikelihood,
                         int idxFitness,
                         int idxRank,
                         int idxFirstMetric,
                         int nObs,
                         QVector<double> &params,
                         int &rank,
                         QVector<double> &mse,
                         QVector<double> &r2,
                         QVector<double> &nse)
{
    QStringList tok = splitFields(line);
    while (!tok.isEmpty() && tok.last().isEmpty())
        tok.removeLast();

    // Minimum tokens: ID + nParams + 3 fixed + nObs*3.
    const int minRequired = 1 + nParams + 3 + nObs * 3;
    if (tok.size() < minRequired)
        return false;

    // Parameters.
    params.resize(nParams);
    for (int k = 0; k < nParams; ++k)
        params[k] = tok.at(1 + k).toDouble();

    rank = tok.at(idxRank).toDouble();

    // Find the start of the metric block. The header places it at
    // idxFirstMetric, but data rows may have inserted a stray empty
    // field. Scan forward from idxFirstMetric, skipping empties,
    // until we have the right number of remaining real tokens.
    int p = idxFirstMetric;
    while (p < tok.size() && tok.at(p).isEmpty())
        ++p;

    // From here on, we expect exactly nObs*3 numeric fields, but the
    // GA emits one extra "," at each triple's boundary so empties may
    // be scattered. Read sequentially, skipping empties.
    mse.resize(nObs);
    r2 .resize(nObs);
    nse.resize(nObs);
    int produced = 0;
    while (p < tok.size() && produced < nObs * 3)
    {
        if (tok.at(p).isEmpty()) { ++p; continue; }
        const double v = tok.at(p).toDouble();
        const int o   = produced / 3;
        const int sub = produced % 3;
        if      (sub == 0) mse[o] = v;
        else if (sub == 1) r2 [o] = v;
        else               nse[o] = v;
        ++produced;
        ++p;
    }
    return produced == nObs * 3;
}

// Percentile of a (will be sorted in place). p in [0, 1].
static double percentile(QVector<double> &a, double p)
{
    if (a.isEmpty()) return 0.0;
    std::sort(a.begin(), a.end());
    if (a.size() == 1) return a.front();
    const double pos = p * (a.size() - 1);
    const int lo = static_cast<int>(std::floor(pos));
    const int hi = static_cast<int>(std::ceil (pos));
    const double frac = pos - lo;
    return a.at(lo) * (1.0 - frac) + a.at(hi) * frac;
}

// ---------------------------------------------------------------------------
// Main parse routine
// ---------------------------------------------------------------------------
bool GaMergedLoader::parse(const QString &text,
                           QVector<CycleSummary> &out,
                           QString &err)
{
    out.clear();

    QTextStream ts(const_cast<QString *>(&text), QIODevice::ReadOnly);

    // We walk the file linearly, keeping track of the current cycle and
    // the current generation. For each cycle, we accumulate the per-
    // individual rows of every generation; when we reach the next
    // generation (or the next cycle / EOF) we know the previous
    // generation has ended. The "terminal generation" for a cycle is
    // the last one encountered before the cycle ends.

    CycleSummary cur;
    bool inCycle = false;
    bool inFinalEnhancements = false;
    QVector<double>           feParams;       // Final Enhancements param values
    QVector<QVector<double>>  feMetrics;      // Final Enhancements metric triplets
    int  curIdxLikelihood = -1;
    int  curIdxFitness    = -1;
    int  curIdxRank       = -1;
    int  curIdxFirstMet   = -1;

    // Per-individual rows for the current generation, kept until
    // generation ends (when we either start a new gen or end the cycle).
    QVector<QVector<double>> genParams;
    QVector<QVector<double>> genMSE, genR2, genNSE;
    QVector<int>             genRank;

    auto finalizeCycle = [&]()
    {
        if (!inCycle) return;
        if (cur.paramNames.isEmpty()) {
            // Cycle never got a column header; skip.
            cur = CycleSummary{};
            inCycle = false;
            return;
        }

        const int nInd = genParams.size();
        const int nP   = cur.paramNames.size();
        const int nO   = cur.observationNames.size();

        // ---- Best individual: prefer Final Enhancements ----------------
        if (feParams.size() == nP)
        {
            cur.bestParams = feParams;
            cur.bestMSE.fill(0.0, nO);
            cur.bestR2 .fill(0.0, nO);
            cur.bestNSE.fill(0.0, nO);
            const int upTo = std::min(nO, static_cast<int>(feMetrics.size()));
            for (int o = 0; o < upTo; ++o)
            {
                if (feMetrics.at(o).size() >= 3)
                {
                    cur.bestMSE[o] = feMetrics.at(o).at(0);
                    cur.bestR2 [o] = feMetrics.at(o).at(1);
                    cur.bestNSE[o] = feMetrics.at(o).at(2);
                }
            }
        }
        else if (nInd > 0)
        {
            // Fallback: Rank == 1 in last GA generation.
            int best = -1;
            for (int i = 0; i < nInd; ++i)
                if (genRank.at(i) == 1) { best = i; break; }
            if (best < 0) best = 0;
            cur.bestParams = genParams.at(best);
            cur.bestMSE    = genMSE   .at(best);
            cur.bestR2     = genR2    .at(best);
            cur.bestNSE    = genNSE   .at(best);
        }
        else
        {
            // No data at all — skip cycle.
            cur = CycleSummary{};
            inCycle = false;
            return;
        }

        // ---- Population spread from terminal GA generation -------------
        if (nInd > 0)
        {
            cur.paramMin.resize(nP);
            cur.paramMax.resize(nP);
            cur.paramP10.resize(nP);
            cur.paramP90.resize(nP);
            for (int k = 0; k < nP; ++k)
            {
                QVector<double> col;
                col.reserve(nInd);
                for (int i = 0; i < nInd; ++i)
                    col.push_back(genParams.at(i).at(k));
                cur.paramMin[k] = *std::min_element(col.begin(), col.end());
                cur.paramMax[k] = *std::max_element(col.begin(), col.end());
                cur.paramP10[k] = percentile(col, 0.10);
                cur.paramP90[k] = percentile(col, 0.90);
            }
        }

        // ---- "Used" observations (non-zero metric for best) -----------
        cur.usedObsIndices.clear();
        for (int o = 0; o < nO; ++o)
        {
            const bool empty =
                cur.bestMSE.at(o) == 0.0 &&
                cur.bestR2 .at(o) == 0.0 &&
                cur.bestNSE.at(o) == 0.0;
            if (!empty) cur.usedObsIndices.push_back(o);
        }

        out.push_back(cur);

        // Reset cycle state.
        cur = CycleSummary{};
        inCycle = false;
        inFinalEnhancements = false;
        feParams.clear();
        feMetrics.clear();
        genParams.clear(); genMSE.clear(); genR2.clear();
        genNSE.clear();    genRank.clear();
        curIdxLikelihood = curIdxFitness = curIdxRank = curIdxFirstMet = -1;
    };

    while (!ts.atEnd())
    {
        const QString rawLine = ts.readLine();
        const QString line    = rawLine.trimmed();
        if (line.isEmpty()) continue;

        // 1. Cycle header.
        {
            int cyc; QDateTime tsh; double tn;
            if (parseCycleHeader(line, cyc, tsh, tn))
            {
                finalizeCycle();             // flush any in-progress cycle
                cur.cycle     = cyc;
                cur.timestamp = tsh;
                cur.tNow      = tn;
                inCycle       = true;
                inFinalEnhancements = false;
                continue;
            }
        }

        // 2. Generation header.
        if (line.startsWith("Generation:"))
        {
            // Close out any in-progress generation; only the LAST
            // generation's accumulators survive to finalizeCycle().
            genParams.clear(); genMSE.clear(); genR2.clear();
            genNSE.clear();    genRank.clear();
            inFinalEnhancements = false;
            continue;
        }

        // 3. Final Enhancements header.
        if (line == "Final Enhancements")
        {
            inFinalEnhancements = true;
            feParams.clear();
            feMetrics.clear();
            continue;
        }

        // 4. Column header.
        if (line.startsWith("ID,") || line.startsWith("ID ,"))
        {
            QStringList paramNames, obsNames;
            int il, ifn, ir, im;
            if (parseColumnHeader(line, paramNames, obsNames,
                                  il, ifn, ir, im))
            {
                if (cur.paramNames.isEmpty())
                {
                    cur.paramNames       = paramNames;
                    cur.observationNames = obsNames;
                }
                curIdxLikelihood = il;
                curIdxFitness    = ifn;
                curIdxRank       = ir;
                curIdxFirstMet   = im;
                inFinalEnhancements = false;
                continue;
            }
            continue;
        }

        // 5. Final Enhancements body.
        if (inFinalEnhancements)
        {
            QStringList tok = splitFields(line);
            while (!tok.isEmpty() && tok.last().isEmpty())
                tok.removeLast();
            if (tok.isEmpty()) continue;

            // Param-value line: starts with a known param name.
            const bool isParamLine =
                !cur.paramNames.isEmpty() &&
                cur.paramNames.contains(tok.front());
            if (isParamLine && tok.size() >= 2)
            {
                feParams.push_back(tok.at(1).toDouble());
                continue;
            }

            // Metric-triplet line: exactly three numeric fields.
            if (tok.size() == 3)
            {
                bool ok0 = false, ok1 = false, ok2 = false;
                const double a = tok.at(0).toDouble(&ok0);
                const double b = tok.at(1).toDouble(&ok1);
                const double c = tok.at(2).toDouble(&ok2);
                if (ok0 && ok1 && ok2)
                {
                    QVector<double> trip{a, b, c};
                    feMetrics.push_back(trip);
                }
            }
            continue;
        }

        // 6. GA data row.
        if (!cur.paramNames.isEmpty() && curIdxRank >= 0)
        {
            QVector<double> params, mse, r2, nse;
            int rank = 0;
            if (parseDataRow(line,
                             cur.paramNames.size(),
                             curIdxLikelihood, curIdxFitness, curIdxRank,
                             curIdxFirstMet,
                             cur.observationNames.size(),
                             params, rank, mse, r2, nse))
            {
                genParams.push_back(params);
                genMSE   .push_back(mse);
                genR2    .push_back(r2);
                genNSE   .push_back(nse);
                genRank  .push_back(rank);
            }
        }
    }

    // Flush the final cycle.
    finalizeCycle();

    if (out.isEmpty())
    {
        err = "ga_output_merged.txt parsed but produced no cycles.";
        return false;
    }
    return true;
}
