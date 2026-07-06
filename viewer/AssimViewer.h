// AssimViewer.h
//
// Assimilation-mode top-level window. Periodically refetches the
// per-cycle calibration history from the deployment's
// outputs/calibration/ directory — ga_output_merged.txt in GA mode,
// posterior_history.jsonl in MCMC mode — and renders tabs:
//
//   - Fitness (GA mode):    two side-by-side charts (NSE and NMSE =
//                 1 - NSE), one line per "used" observation (an
//                 observation whose MSE/R2/NSE are not all zero in the
//                 best individual). NMSE uses a log Y axis.
//
//   - Diagnostics (MCMC mode): per-cycle sampler health — effective
//                 sample size, plateaued fraction, acceptance rate,
//                 pool size.
//
//   - Parameters: one chart per calibrated parameter, laid out in a
//                 wrapping grid inside a scroll area. Each chart shows
//                 the best/point-estimate trajectory (a line) and a
//                 band: GA population p10-p90, or the posterior 10-90%
//                 credible interval (converged cycles only) in MCMC
//                 mode.
//
// X-axis can be toggled between "cycle index" and "simulated time
// t_now" (rendered as a calendar date via OHQ day-serial conversion).
//
// Data lifecycle:
//   - The window owns one loader (GaMergedLoader or
//     PosteriorHistoryLoader, chosen by which URL key the config
//     provides) and a single QTimer.
//   - On each timer tick (or manual refresh), the loader fetches the
//     history and emits `loaded(cycles)`. The viewer wipes and
//     rebuilds chart contents from the new vector (the chart structure
//     stays put across refreshes; only the series data changes).

#pragma once

#include "CsvLoader.h"
#include "GaMergedLoader.h"
#include "PosteriorHistoryLoader.h"
#include "RealizationCILoader.h"

#include <QJsonObject>
#include <QMainWindow>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <QtCharts/QChartGlobal>

class QButtonGroup;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QVBoxLayout;
class QWidget;

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
QT_CHARTS_BEGIN_NAMESPACE
    class QAbstractAxis;
class QAreaSeries;
class QChart;
class QChartView;
class QDateTimeAxis;
class QLineSeries;
class QLogValueAxis;
class QScatterSeries;
class QValueAxis;
QT_CHARTS_END_NAMESPACE
    QT_CHARTS_USE_NAMESPACE
#else
class QAbstractAxis;
class QAreaSeries;
class QChart;
class QChartView;
class QDateTimeAxis;
class QLineSeries;
class QLogValueAxis;
class QScatterSeries;
class QValueAxis;
#endif

    // One observation's line in the Fitness tab, with one QLineSeries per
    // enabled metric panel (parallel to m_fitnessCharts). The series at
    // index k belongs to m_fitnessCharts[k].chart.
    struct FitnessLineSpec
{
    int          obsIndex   = -1;     // index into CycleSummary::observationNames
    QString      name;
    QVector<QLineSeries *> series;    // one per metric panel
};

// One chart panel in the Fitness tab — for example "nse" or "nmse".
struct FitnessChartPanel
{
    QString         metric;           // "nse" | "nmse"
    QChart         *chart      = nullptr;
    QChartView     *view       = nullptr;
    QDateTimeAxis  *dateAxis   = nullptr;
    QValueAxis     *cycleAxis  = nullptr;
    // Y axis: linear (QValueAxis) for nse, log (QLogValueAxis) for nmse.
    // Stored generically as QAbstractAxis* with a flag.
    QAbstractAxis  *yAxis      = nullptr;
    bool            yIsLog     = false;
};

// One chart per sampler diagnostic in the Diagnostics tab (MCMC mode).
struct DiagPanel
{
    QString         title;

    QChart         *chart     = nullptr;
    QChartView     *view      = nullptr;
    QLineSeries    *series    = nullptr;

    QDateTimeAxis  *dateAxis  = nullptr;
    QValueAxis     *cycleAxis = nullptr;
    QValueAxis     *yAxis     = nullptr;
};

// One chart panel per calibrated parameter in the Parameters tab.
struct ParamPanel
{
    int             paramIndex = -1;
    QString         name;

    QChart         *chart      = nullptr;
    QChartView     *view       = nullptr;

    QLineSeries    *bestSeries = nullptr;
    QLineSeries    *p10Series  = nullptr;     // lower bound (hidden)
    QLineSeries    *p90Series  = nullptr;     // upper bound (hidden)
    QAreaSeries    *bandArea   = nullptr;     // p10-p90 fill (GA mode)

    // MCMC mode: the band exists only over contiguous runs of CONVERGED
    // cycles; each run is its own area series, rebuilt on every data
    // refresh (provisional cycles break the band — transit dispersion is
    // never rendered as posterior width).
    QVector<QAreaSeries*> bandSegments;

    // Either dateAxis or cycleAxis is attached at any given time,
    // depending on the x-axis toggle. We keep both around so the
    // toggle is cheap.
    QDateTimeAxis  *dateAxis   = nullptr;
    QValueAxis     *cycleAxis  = nullptr;
    QValueAxis     *yAxis      = nullptr;
};

// One chart panel per series in the Comparison tab. Each panel
// overlays an observed scatter series (from the truth twin's
// selected_output.csv) and a modeled line series (from the
// assimilation twin's selected_output.csv) on shared axes.
struct ComparisonPanel
{
    QString          name;
    QChart          *chart        = nullptr;
    QChartView      *view         = nullptr;
    QScatterSeries  *observedSer  = nullptr;
    QLineSeries     *modeledSer   = nullptr;
    QDateTimeAxis   *xAxis        = nullptr;
    QValueAxis      *yAxis        = nullptr;
    QAreaSeries    *ciBand      = nullptr;   // realization 2.5–97.5 band
    QLineSeries    *ciLo        = nullptr;
    QLineSeries    *ciHi        = nullptr;
};

class AssimViewer : public QMainWindow
{
    Q_OBJECT

public:
    explicit AssimViewer(const QJsonObject &rootConfig,
                         const QJsonObject &assimConfig,
                         const QUrl        &configBaseUrl,
                         QWidget *parent = nullptr);

private slots:
    void onRefreshClicked();
    void onIntervalChanged(int seconds);
    void onLoaded(const QVector<CycleSummary> &cycles);
    void onFailed(const QString &errorMessage);
    void onXAxisModeChanged();

    void onObservedLoaded(const QVector<CsvSeries> &series);
    void onModeledLoaded (const QVector<CsvSeries> &series);
    void onObservedFailed(const QString &errorMessage);
    void onModeledFailed (const QString &errorMessage);
    void onRealizationsLoaded(const QHash<QString, RealizationBand> &bands);

private:
    // Build (once) the chart shells for the fitness tab — two charts
    // (NSE, NMSE), each with one line per used observation. Called the
    // first time we see a non-empty cycles list, or whenever the set
    // of "used" observation names changes.
    void rebuildFitnessCharts(const QVector<CycleSummary> &cycles);

    // Build (once) the per-parameter chart grid in the parameters tab.
    void rebuildParameterCharts(const QVector<CycleSummary> &cycles);
    void buildDiagnosticsTab();
    void updateDiagnosticsSeries(const QVector<CycleSummary> &cycles);

    // Replace data in the existing charts from `cycles`. Cheap; called
    // every refresh.
    void updateFitnessSeries  (const QVector<CycleSummary> &cycles);
    void updateParameterSeries(const QVector<CycleSummary> &cycles);

    // Apply the current x-axis mode (cycle vs t_now) to all chart axes.
    void applyXAxisToAllCharts();

    // Build (or rebuild) the comparison-tab panels when the set of
    // series names changes. Called the first time both CSVs are seen
    // and whenever the observed/modeled name lists change shape.
    void rebuildComparisonPanels(const QVector<CsvSeries> &observed,
                                 const QVector<CsvSeries> &modeled);

    // Push fresh observed + modeled points into the existing panels.
    void updateComparisonPanels();

    // Helpers.
    static qreal cycleX(const CycleSummary &c, bool useDate);
    static QString shortenName(const QString &full);

    // ----- state -----
    QUrl              m_mergedUrl;                       // GA mode
    QUrl              m_historyUrl;                      // MCMC mode
    bool              m_mcmcMode     = false;
    GaMergedLoader        *m_loader        = nullptr;    // GA mode only
    PosteriorHistoryLoader *m_historyLoader = nullptr;   // MCMC mode only

    QTimer            m_timer;
    int               m_refreshSeconds = 10;

    QVector<CycleSummary> m_lastCycles;
    QVector<DiagPanel>    m_diagPanels;       // MCMC mode only
    QStringList           m_lastUsedNames;     // ordered union of used-obs names
    QStringList           m_lastParamNames;    // schema for the parameters tab

    // X-axis mode: false = cycle index, true = simulated time (date).
    bool m_useDateAxis = true;

    // Parameter band visibility (from assimilation.parameter_panel.show_population_band).
    // Read once from config at startup; no live UI toggle.
    bool m_showParamBand = true;

    // ----- UI -----
    QLabel       *m_statusLabel     = nullptr;
    QSpinBox     *m_intervalSpin    = nullptr;
    QPushButton  *m_refreshBtn      = nullptr;
    QPushButton  *m_xAxisCycleBtn   = nullptr;
    QPushButton  *m_xAxisDateBtn    = nullptr;
    QButtonGroup *m_xAxisGroup      = nullptr;

    QTabWidget   *m_tabs            = nullptr;

    // Fitness tab. One QChart per enabled metric (in config order).
    QWidget                  *m_fitnessTabHost = nullptr;
    QHBoxLayout              *m_fitnessLayout  = nullptr;
    QVector<FitnessChartPanel> m_fitnessCharts;
    QStringList                m_enabledMetrics;   // ordered, lowercased

    QVector<FitnessLineSpec> m_fitnessLines;

    // Parameters tab.
    QWidget      *m_paramTabHost    = nullptr;
    QGridLayout  *m_paramGrid       = nullptr;
    QVector<ParamPanel> m_paramPanels;

    // Comparison tab.
    QUrl                 m_observedCsvUrl;
    QUrl                 m_modeledCsvUrl;
    CsvLoader            m_observedLoader;
    CsvLoader            m_modeledLoader;
    QVector<CsvSeries>   m_lastObserved;
    QVector<CsvSeries>   m_lastModeled;
    bool                 m_haveObserved = false;
    bool                 m_haveModeled  = false;
    QWidget             *m_compTabHost  = nullptr;
    QVBoxLayout         *m_compLayout   = nullptr;
    QVector<ComparisonPanel> m_compPanels;

    RealizationCILoader m_realizationLoader;
    QHash<QString, RealizationBand> m_lastBands;
    QString m_realizationUrlTemplate;
};
