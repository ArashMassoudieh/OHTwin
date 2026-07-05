// AssimViewer.cpp

#include "AssimViewer.h"
#include "OhqTime.h"

#include <QBrush>
#include <QButtonGroup>
#include <QColor>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <QtCharts/QAreaSeries>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLegend>
#include <QtCharts/QLegendMarker>
#include <QtCharts/QLineSeries>
#include <QtCharts/QLogValueAxis>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Style constants
// ---------------------------------------------------------------------------
static const QStringList kLineColors = {
    "#2D7FF9", "#10B981", "#F59E0B", "#EF4444",
    "#8B5CF6", "#EC4899", "#0EA5E9", "#84CC16",
    "#F97316", "#06B6D4",
};

static const QColor kGridColor   ("#EDF0F4");
static const QColor kLabelColor  ("#6B7684");
static const QColor kTitleColor  ("#1E2A38");
static const QColor kBandColor   ("#2D7FF9");   // alpha applied below

static QString statusHtml(const QString &color, const QString &text)
{
    return QString("<span style='color:%1; font-size:14px;'>&#9679;</span>"
                   " <span>%2</span>").arg(color, text.toHtmlEscaped());
}

// Resolve a relative URL against configBaseUrl (mirrors MainWindow helper).
static QUrl resolvedConfigUrl(const QUrl &configUrl, const QString &value)
{
    if (value.trimmed().isEmpty()) return {};
    return configUrl.resolved(QUrl(value.trimmed()));
}

// Strip parenthetical units off observation names so legend labels fit.
//   "Underdrain flow (m3/day)"  ->  "Underdrain flow"
QString AssimViewer::shortenName(const QString &full)
{
    const int paren = full.indexOf('(');
    if (paren < 0) return full;
    return full.left(paren).trimmed();
}

// Convert one cycle's x position based on the current axis mode.
//   useDate == true  -> ms-since-epoch from OHQ day-serial
//   useDate == false -> cycle index
qreal AssimViewer::cycleX(const CycleSummary &c, bool useDate)
{
    if (useDate) return static_cast<qreal>(ohqSerialToMsEpoch(c.tNow));
    return static_cast<qreal>(c.cycle);
}

// ---------------------------------------------------------------------------
// Constructor: build the UI shell. Charts are created lazily on first load.
// ---------------------------------------------------------------------------
AssimViewer::AssimViewer(const QJsonObject &rootConfig,
                         const QJsonObject &assimConfig,
                         const QUrl        &configBaseUrl,
                         QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("OHTwin Assimilation Viewer");

    // ----- absorb config -----
    {
        // Loader selection by URL key: posterior_history_url => MCMC
        // mode (PosteriorHistoryLoader), ga_merged_url => GA mode
        // (GaMergedLoader). If both are present, MCMC wins — mixing two
        // deployments' histories in one window is never intended.
        const QString hu = assimConfig.value("posterior_history_url").toString();
        const QString u  = assimConfig.value("ga_merged_url").toString();
        m_historyUrl = resolvedConfigUrl(configBaseUrl, hu);
        m_mergedUrl  = resolvedConfigUrl(configBaseUrl, u);
        m_mcmcMode   = !m_historyUrl.isEmpty();
        if (m_mcmcMode && !m_mergedUrl.isEmpty())
            qWarning("AssimViewer: both posterior_history_url and "
                     "ga_merged_url configured; using MCMC history and "
                     "ignoring ga_merged_url.");

        const QString obs = assimConfig.value("observed_csv_url").toString();
        m_observedCsvUrl = resolvedConfigUrl(configBaseUrl, obs);
        const QString mod = assimConfig.value("modeled_csv_url").toString();
        m_modeledCsvUrl = resolvedConfigUrl(configBaseUrl, mod);

        if (assimConfig.contains("refresh_seconds"))
            m_refreshSeconds = assimConfig.value("refresh_seconds").toInt(m_refreshSeconds);
        else if (rootConfig.contains("refresh_seconds"))
            m_refreshSeconds = rootConfig.value("refresh_seconds").toInt(m_refreshSeconds);

        const QString xa = assimConfig.value("x_axis").toString("t_now").trimmed().toLower();
        m_useDateAxis = (xa != "cycle");

        // Fitness metrics — config-driven list. Order in JSON is the
        // order panels appear in the Fitness tab. Default: NSE then
        // NMSE (= 1 - NSE on a log scale). Unknown metrics get
        // warned about and dropped.
        const QJsonArray mArr = assimConfig.value("fitness_metrics").toArray();
        if (mArr.isEmpty())
        {
            m_enabledMetrics = QStringList{ "nse", "nmse" };
        }
        else
        {
            for (const QJsonValue &v : mArr)
            {
                const QString m = v.toString().trimmed().toLower();
                if (m == "nse" || m == "nmse")
                    m_enabledMetrics.push_back(m);
                else
                    qWarning() << "[AssimViewer] Unknown fitness metric:" << m
                               << "(supported: nse, nmse)";
            }
            if (m_enabledMetrics.isEmpty())
            {
                qWarning() << "[AssimViewer] No supported metrics in fitness_metrics; "
                              "falling back to default (nse, nmse).";
                m_enabledMetrics = QStringList{ "nse", "nmse" };
            }
        }

        // Parameter-band visibility from assimilation.parameter_panel.show_population_band.
        const QJsonObject paramPanel = assimConfig.value("parameter_panel").toObject();
        m_showParamBand =
            paramPanel.value("show_population_band").toBool(true);
    }

    // ----- central layout -----
    auto *central = new QWidget(this);
    auto *root    = new QVBoxLayout(central);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(14);

    // ----- header -----
    auto *title = new QLabel("OHTwin Assimilation Viewer");
    title->setObjectName("HeaderTitle");
    auto *subtitle = new QLabel(
        "Calibration diagnostics — fitness and parameter convergence per cycle");
    subtitle->setObjectName("HeaderSubtitle");
    root->addWidget(title);
    root->addWidget(subtitle);

    // ----- top bar -----
    auto *topBarCard = new QFrame();
    topBarCard->setObjectName("TopBarCard");
    auto *topBar = new QHBoxLayout(topBarCard);
    topBar->setContentsMargins(14, 12, 14, 12);
    topBar->setSpacing(10);

    topBar->addWidget(new QLabel("Refresh (s)"));
    m_intervalSpin = new QSpinBox();
    m_intervalSpin->setRange(2, 24 * 3600);
    m_intervalSpin->setValue(m_refreshSeconds);
    m_intervalSpin->setFixedWidth(90);
    topBar->addWidget(m_intervalSpin);

    topBar->addSpacing(8);
    topBar->addWidget(new QLabel("X-axis"));
    m_xAxisCycleBtn = new QPushButton("Cycle");
    m_xAxisDateBtn  = new QPushButton("Time");
    m_xAxisCycleBtn->setCheckable(true);
    m_xAxisDateBtn ->setCheckable(true);
    m_xAxisCycleBtn->setChecked(!m_useDateAxis);
    m_xAxisDateBtn ->setChecked( m_useDateAxis);
    m_xAxisGroup = new QButtonGroup(this);
    m_xAxisGroup->setExclusive(true);
    m_xAxisGroup->addButton(m_xAxisCycleBtn);
    m_xAxisGroup->addButton(m_xAxisDateBtn);
    topBar->addWidget(m_xAxisCycleBtn);
    topBar->addWidget(m_xAxisDateBtn);

    topBar->addStretch(1);

    m_refreshBtn = new QPushButton("Refresh");
    topBar->addWidget(m_refreshBtn);

    root->addWidget(topBarCard);

    // ----- status line -----
    m_statusLabel = new QLabel();
    m_statusLabel->setObjectName("StatusLabel");
    m_statusLabel->setTextFormat(Qt::RichText);
    m_statusLabel->setText(statusHtml("#9CA3AF", "Idle"));
    root->addWidget(m_statusLabel);

    // ----- tabs -----
    m_tabs = new QTabWidget();

    // ----- Fitness tab: one QChartView per enabled metric, side by side -----
    {
        m_fitnessTabHost = new QWidget();
        m_fitnessLayout  = new QHBoxLayout(m_fitnessTabHost);
        m_fitnessLayout->setContentsMargins(0, 0, 0, 0);
        m_fitnessLayout->setSpacing(12);

        for (const QString &metric : m_enabledMetrics)
        {
            FitnessChartPanel fp;
            fp.metric = metric;
            fp.yIsLog = (metric == "nmse");
            fp.chart  = new QChart();
            fp.chart->setTitleFont(QFont("Inter", 12, QFont::DemiBold));
            fp.chart->setTitleBrush(QBrush(kTitleColor));
            fp.chart->setBackgroundBrush(QBrush(QColor("#FFFFFF")));
            fp.chart->setBackgroundPen(QPen(QColor("#E1E5EB")));
            fp.chart->setBackgroundRoundness(12);
            fp.chart->setMargins(QMargins(14, 10, 14, 10));
            fp.chart->setPlotAreaBackgroundVisible(false);
            fp.chart->legend()->setVisible(true);
            fp.chart->legend()->setAlignment(Qt::AlignBottom);
            fp.chart->legend()->setLabelColor(kLabelColor);
            if (metric == "nse")
                fp.chart->setTitle("NSE  (best individual, per observation)");
            else if (metric == "nmse")
                fp.chart->setTitle("NMSE = 1 - NSE  (log axis)");

            fp.view = new QChartView(fp.chart);
            fp.view->setRenderHint(QPainter::Antialiasing);
            fp.view->setFrameShape(QFrame::NoFrame);
            fp.view->setStyleSheet("background: transparent;");
            fp.view->setMinimumHeight(360);
            m_fitnessLayout->addWidget(fp.view, 1);

            m_fitnessCharts.push_back(fp);
        }

        if (!m_mcmcMode)
            m_tabs->addTab(m_fitnessTabHost, "Fitness");
    }

    // ----- Diagnostics tab (MCMC mode): sampler health per cycle -----
    if (m_mcmcMode)
        buildDiagnosticsTab();

    // ----- Parameters tab: grid of charts in a scroll area -----
    {
        auto *scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);

        auto *outer  = new QWidget();
        auto *outerL = new QVBoxLayout(outer);
        outerL->setContentsMargins(0, 0, 0, 0);
        outerL->setSpacing(6);

        // The band means different things in the two modes; say which.
        auto *bandCaption = new QLabel(
            m_mcmcMode
                ? "Band: posterior 10\u201390% credible interval "
                  "(converged cycles only \u2014 provisional cycles show "
                  "the point estimate with no band)"
                : "Band: GA population p10\u2013p90 (terminal generation)");
        bandCaption->setStyleSheet("color:#6B7280; font-size:11px;");
        bandCaption->setVisible(m_showParamBand);
        outerL->addWidget(bandCaption);

        m_paramTabHost = new QWidget();
        m_paramGrid    = new QGridLayout(m_paramTabHost);
        m_paramGrid->setContentsMargins(0, 0, 0, 0);
        m_paramGrid->setHorizontalSpacing(12);
        m_paramGrid->setVerticalSpacing(12);
        outerL->addWidget(m_paramTabHost, 1);

        scroll->setWidget(outer);
        m_tabs->addTab(scroll, "Parameters");
    }

    // ----- Comparison tab: stacked charts in a scroll area -----
    {
        auto *scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);

        m_compTabHost = new QWidget();
        m_compLayout  = new QVBoxLayout(m_compTabHost);
        m_compLayout->setContentsMargins(0, 0, 0, 0);
        m_compLayout->setSpacing(14);

        scroll->setWidget(m_compTabHost);
        m_tabs->addTab(scroll, "Comparison");
    }

    root->addWidget(m_tabs, 1);
    setCentralWidget(central);
    resize(1280, 860);

    // ----- loader & timer -----
    if (m_mcmcMode)
    {
        setWindowTitle("OHTwin Assimilation Viewer (MCMC)");
        m_historyLoader = new PosteriorHistoryLoader(this);
        m_historyLoader->setHistoryUrl(m_historyUrl);
        connect(m_historyLoader, &PosteriorHistoryLoader::loaded,
                this, &AssimViewer::onLoaded);
        connect(m_historyLoader, &PosteriorHistoryLoader::failed,
                this, &AssimViewer::onFailed);
    }
    else
    {
        m_loader = new GaMergedLoader(this);
        m_loader->setMergedUrl(m_mergedUrl);
        connect(m_loader, &GaMergedLoader::loaded, this, &AssimViewer::onLoaded);
        connect(m_loader, &GaMergedLoader::failed, this, &AssimViewer::onFailed);
    }
    connect(&m_timer, &QTimer::timeout,        this, &AssimViewer::onRefreshClicked);
    connect(m_refreshBtn, &QPushButton::clicked,
            this, &AssimViewer::onRefreshClicked);
    connect(m_intervalSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &AssimViewer::onIntervalChanged);
    connect(m_xAxisCycleBtn, &QPushButton::clicked,
            this, &AssimViewer::onXAxisModeChanged);
    connect(m_xAxisDateBtn,  &QPushButton::clicked,
            this, &AssimViewer::onXAxisModeChanged);

    connect(&m_observedLoader, &CsvLoader::loaded,
            this, &AssimViewer::onObservedLoaded);
    connect(&m_observedLoader, &CsvLoader::failed,
            this, &AssimViewer::onObservedFailed);
    connect(&m_modeledLoader,  &CsvLoader::loaded,
            this, &AssimViewer::onModeledLoaded);
    connect(&m_modeledLoader,  &CsvLoader::failed,
            this, &AssimViewer::onModeledFailed);

    m_timer.setInterval(m_refreshSeconds * 1000);
    m_timer.start();
    onRefreshClicked();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
void AssimViewer::onRefreshClicked()
{
    m_statusLabel->setText(statusHtml("#F59E0B", "Fetching …"));
    if (m_mcmcMode) m_historyLoader->fetch();
    else            m_loader->fetch();
    if (!m_observedCsvUrl.isEmpty()) m_observedLoader.fetch(m_observedCsvUrl);
    if (!m_modeledCsvUrl .isEmpty()) m_modeledLoader .fetch(m_modeledCsvUrl);
}

void AssimViewer::onIntervalChanged(int seconds)
{
    m_refreshSeconds = seconds;
    m_timer.setInterval(m_refreshSeconds * 1000);
}

void AssimViewer::onFailed(const QString &err)
{
    m_statusLabel->setText(statusHtml("#EF4444", "Error: " + err));
}

void AssimViewer::onLoaded(const QVector<CycleSummary> &cycles)
{
    if (cycles.isEmpty())
    {
        m_statusLabel->setText(statusHtml("#9CA3AF", "Loaded 0 cycles"));
        return;
    }
    m_lastCycles = cycles;

    // Decide whether structural rebuilds are needed.
    QStringList usedNames;
    for (int i : cycles.last().usedObsIndices)
        usedNames.push_back(cycles.last().observationNames.at(i));

    const QStringList paramNames = cycles.last().paramNames;

    if (usedNames != m_lastUsedNames)
    {
        m_lastUsedNames = usedNames;
        rebuildFitnessCharts(cycles);
    }
    if (paramNames != m_lastParamNames)
    {
        m_lastParamNames = paramNames;
        rebuildParameterCharts(cycles);
    }

    updateFitnessSeries(cycles);
    updateParameterSeries(cycles);
    if (m_mcmcMode) updateDiagnosticsSeries(cycles);
    applyXAxisToAllCharts();

    // The Comparison tab decides whether to show observed dots per
    // panel based on m_lastUsedNames, which we just updated. Re-apply
    // that decision if both CSVs are already in.
    if (m_haveObserved && m_haveModeled)
        updateComparisonPanels();

    const CycleSummary &latest = cycles.last();
    const QDateTime tNowDt =
        QDateTime::fromMSecsSinceEpoch(ohqSerialToMsEpoch(latest.tNow));
    m_statusLabel->setText(statusHtml("#10B981",
                                      QStringLiteral("Cycle %1 · t_now = %2 · %3 cycle(s) loaded")
                                          .arg(latest.cycle)
                                          .arg(tNowDt.toString("yyyy-MM-dd HH:mm"))
                                          .arg(cycles.size())));
}

void AssimViewer::onXAxisModeChanged()
{
    m_useDateAxis = m_xAxisDateBtn->isChecked();
    applyXAxisToAllCharts();
    if (!m_lastCycles.isEmpty())
    {
        updateFitnessSeries(m_lastCycles);
        updateParameterSeries(m_lastCycles);
        if (m_mcmcMode) updateDiagnosticsSeries(m_lastCycles);
    }
}

// ---------------------------------------------------------------------------
// Fitness charts: structural rebuild
// ---------------------------------------------------------------------------
void AssimViewer::rebuildFitnessCharts(const QVector<CycleSummary> &cycles)
{
    // Tear down existing series / axes on every metric chart.
    auto wipe = [](QChart *c)
    {
        const auto axes = c->axes();
        for (auto *a : axes) { c->removeAxis(a); delete a; }
        const auto ss = c->series();
        for (auto *s : ss)   { c->removeSeries(s); delete s; }
    };
    for (FitnessChartPanel &fp : m_fitnessCharts)
    {
        wipe(fp.chart);
        fp.dateAxis = nullptr; fp.cycleAxis = nullptr; fp.yAxis = nullptr;
    }
    m_fitnessLines.clear();

    if (cycles.isEmpty()) return;
    const CycleSummary &ref = cycles.last();

    // Axes per metric panel: both a date axis and a cycle (value)
    // axis (only one visible per the toggle), plus a y axis specific
    // to the metric (linear for nse, log for nmse).
    for (FitnessChartPanel &fp : m_fitnessCharts)
    {
        fp.dateAxis  = new QDateTimeAxis();
        fp.dateAxis->setFormat("yyyy-MM-dd");
        fp.dateAxis->setTitleText("Simulated time");
        fp.cycleAxis = new QValueAxis();
        fp.cycleAxis->setTitleText("Cycle");
        fp.cycleAxis->setLabelFormat("%d");
        for (QAbstractAxis *a : { (QAbstractAxis *)fp.dateAxis,
                                 (QAbstractAxis *)fp.cycleAxis })
        {
            a->setTitleFont(QFont("Inter", 10, QFont::DemiBold));
            a->setLabelsFont(QFont("Inter", 9));
            a->setTitleBrush(QBrush(kLabelColor));
            a->setLabelsColor(kLabelColor);
            a->setGridLineColor(kGridColor);
            a->setLinePenColor(kGridColor);
        }
        fp.chart->addAxis(fp.dateAxis,  Qt::AlignBottom);
        fp.chart->addAxis(fp.cycleAxis, Qt::AlignBottom);
        fp.dateAxis ->setVisible(m_useDateAxis);
        fp.cycleAxis->setVisible(!m_useDateAxis);

        if (fp.metric == "nse")
        {
            auto *ya = new QValueAxis();
            ya->setTitleText("NSE");
            ya->setRange(-1.0, 1.0);
            ya->setLabelFormat("%.2f");
            ya->setTitleFont(QFont("Inter", 10, QFont::DemiBold));
            ya->setLabelsFont(QFont("Inter", 9));
            ya->setTitleBrush(QBrush(kLabelColor));
            ya->setLabelsColor(kLabelColor);
            ya->setGridLineColor(kGridColor);
            fp.chart->addAxis(ya, Qt::AlignLeft);
            fp.yAxis  = ya;
            fp.yIsLog = false;
        }
        else  // nmse
        {
            auto *ya = new QLogValueAxis();
            ya->setBase(10.0);
            ya->setMinorTickCount(8);
            ya->setTitleText("NMSE  (log scale)");
            ya->setLabelFormat("%.2g");
            ya->setRange(1e-3, 1e3);
            ya->setTitleFont(QFont("Inter", 10, QFont::DemiBold));
            ya->setLabelsFont(QFont("Inter", 9));
            ya->setTitleBrush(QBrush(kLabelColor));
            ya->setLabelsColor(kLabelColor);
            ya->setGridLineColor(kGridColor);
            fp.chart->addAxis(ya, Qt::AlignLeft);
            fp.yAxis  = ya;
            fp.yIsLog = true;
        }
    }

    // One line per used observation, on each metric chart.
    for (int k = 0; k < ref.usedObsIndices.size(); ++k)
    {
        const int o = ref.usedObsIndices.at(k);
        const QString fullName  = ref.observationNames.at(o);
        const QString shortName = shortenName(fullName);

        FitnessLineSpec spec;
        spec.obsIndex = o;
        spec.name     = fullName;
        spec.series.reserve(m_fitnessCharts.size());

        const QColor color(kLineColors[k % kLineColors.size()]);
        QPen pen(color);
        pen.setWidthF(2.0);
        pen.setCapStyle(Qt::RoundCap);

        for (FitnessChartPanel &fp : m_fitnessCharts)
        {
            auto *s = new QLineSeries();
            s->setName(shortName);
            s->setPen(pen);
            fp.chart->addSeries(s);

            QAbstractAxis *xAx = m_useDateAxis
                                     ? (QAbstractAxis *)fp.dateAxis
                                     : (QAbstractAxis *)fp.cycleAxis;
            s->attachAxis(xAx);
            s->attachAxis(fp.yAxis);

            spec.series.push_back(s);
        }

        m_fitnessLines.push_back(spec);
    }
}

// ---------------------------------------------------------------------------
// Fitness charts: refresh data
// ---------------------------------------------------------------------------
void AssimViewer::updateFitnessSeries(const QVector<CycleSummary> &cycles)
{
    if (cycles.isEmpty() || m_fitnessLines.isEmpty() || m_fitnessCharts.isEmpty())
        return;

    // Per metric panel, track y range as we walk the cycles so we can
    // expand the axis afterwards.
    QVector<double> metricMin(m_fitnessCharts.size(),
                              std::numeric_limits<double>::max());
    QVector<double> metricMax(m_fitnessCharts.size(),
                              std::numeric_limits<double>::lowest());

    for (FitnessLineSpec &spec : m_fitnessLines)
    {
        // Build a points list per metric.
        QVector<QVector<QPointF>> pts(m_fitnessCharts.size());
        for (QVector<QPointF> &p : pts) p.reserve(cycles.size());

        for (const CycleSummary &c : cycles)
        {
            if (spec.obsIndex >= c.bestNSE.size()) continue;
            const double nse  = c.bestNSE.at(spec.obsIndex);
            const double nmse = 1.0 - nse;
            const qreal  x    = cycleX(c, m_useDateAxis);

            for (int m = 0; m < m_fitnessCharts.size(); ++m)
            {
                const QString &metric = m_fitnessCharts.at(m).metric;
                double y = 0.0;
                if (metric == "nse")  y = nse;
                else if (metric == "nmse")
                {
                    // QLogValueAxis can't show <= 0; clamp.
                    y = (nmse > 0.0) ? nmse : 1e-6;
                }
                pts[m].push_back(QPointF(x, y));
                metricMin[m] = std::min(metricMin[m], y);
                metricMax[m] = std::max(metricMax[m], y);
            }
        }

        for (int m = 0; m < m_fitnessCharts.size(); ++m)
            if (m < spec.series.size() && spec.series.at(m))
                spec.series[m]->replace(pts[m]);
    }

    // Widen Y axes a bit.
    for (int m = 0; m < m_fitnessCharts.size(); ++m)
    {
        FitnessChartPanel &fp = m_fitnessCharts[m];
        if (!fp.yAxis) continue;
        const double mn = metricMin[m];
        const double mx = metricMax[m];
        if (mx <= mn) continue;

        if (fp.yIsLog)
        {
            if (mn > 0.0)
                static_cast<QLogValueAxis *>(fp.yAxis)->setRange(mn * 0.5, mx * 2.0);
        }
        else
        {
            const double pad = (mx - mn) * 0.10 + 1e-6;
            // For NSE specifically, keep the [-1, 1] floor visible.
            if (fp.metric == "nse")
            {
                static_cast<QValueAxis *>(fp.yAxis)->setRange(
                    std::min(mn - pad, -1.0),
                    std::max(mx + pad,  1.0));
            }
            else
            {
                static_cast<QValueAxis *>(fp.yAxis)->setRange(mn - pad, mx + pad);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Parameter charts: structural rebuild
// ---------------------------------------------------------------------------
void AssimViewer::rebuildParameterCharts(const QVector<CycleSummary> &cycles)
{
    // Wipe existing.
    for (ParamPanel &p : m_paramPanels)
    {
        if (p.view) { m_paramGrid->removeWidget(p.view); p.view->deleteLater(); }
    }
    m_paramPanels.clear();

    if (cycles.isEmpty()) return;
    const CycleSummary &ref = cycles.last();

    const int nCols = 2;
    int row = 0, col = 0;

    for (int k = 0; k < ref.paramNames.size(); ++k)
    {
        ParamPanel pp;
        pp.paramIndex = k;
        pp.name       = ref.paramNames.at(k);

        pp.chart = new QChart();
        pp.chart->setTitle(pp.name);
        pp.chart->setTitleFont(QFont("Inter", 11, QFont::DemiBold));
        pp.chart->setTitleBrush(QBrush(kTitleColor));
        pp.chart->setBackgroundBrush(QBrush(QColor("#FFFFFF")));
        pp.chart->setBackgroundPen(QPen(QColor("#E1E5EB")));
        pp.chart->setBackgroundRoundness(12);
        pp.chart->setMargins(QMargins(10, 8, 10, 8));
        pp.chart->setPlotAreaBackgroundVisible(false);
        pp.chart->legend()->setVisible(false);   // single-parameter chart; no need

        // Axes (both x options, only one visible at a time).
        pp.dateAxis  = new QDateTimeAxis();
        pp.dateAxis->setFormat("yyyy-MM-dd");
        pp.cycleAxis = new QValueAxis();
        pp.cycleAxis->setLabelFormat("%d");
        pp.yAxis     = new QValueAxis();
        for (QAbstractAxis *a : { (QAbstractAxis*)pp.dateAxis,
                                 (QAbstractAxis*)pp.cycleAxis,
                                 (QAbstractAxis*)pp.yAxis })
        {
            a->setLabelsFont(QFont("Inter", 9));
            a->setLabelsColor(kLabelColor);
            a->setGridLineColor(kGridColor);
            a->setLinePenColor(kGridColor);
        }
        pp.chart->addAxis(pp.dateAxis,  Qt::AlignBottom);
        pp.chart->addAxis(pp.cycleAxis, Qt::AlignBottom);
        pp.chart->addAxis(pp.yAxis,     Qt::AlignLeft);
        pp.dateAxis ->setVisible(m_useDateAxis);
        pp.cycleAxis->setVisible(!m_useDateAxis);

        QAbstractAxis *xAx = m_useDateAxis ? (QAbstractAxis*)pp.dateAxis
                                           : (QAbstractAxis*)pp.cycleAxis;

        // p10-p90 area (built from two hidden line series).
        // Created only when the config opts in (default true).
        // GA mode only: in MCMC mode the band is a set of per-run
        // segments rebuilt in updateParameterSeries.
        if (m_showParamBand && !m_mcmcMode)
        {
            pp.p10Series = new QLineSeries();
            pp.p90Series = new QLineSeries();
            pp.bandArea  = new QAreaSeries(pp.p90Series, pp.p10Series);
            QColor bandFill = kBandColor; bandFill.setAlpha(60);
            pp.bandArea->setBrush(QBrush(bandFill));
            pp.bandArea->setPen(QPen(Qt::NoPen));
            pp.chart->addSeries(pp.bandArea);
            pp.bandArea->attachAxis(xAx);
            pp.bandArea->attachAxis(pp.yAxis);
        }

        // Best line.
        pp.bestSeries = new QLineSeries();
        QPen pen{QColor(kLineColors[k % kLineColors.size()])};
        pen.setWidthF(2.4);
        pen.setCapStyle(Qt::RoundCap);
        pp.bestSeries->setPen(pen);
        pp.chart->addSeries(pp.bestSeries);
        pp.bestSeries->attachAxis(xAx);
        pp.bestSeries->attachAxis(pp.yAxis);

        pp.view = new QChartView(pp.chart);
        pp.view->setRenderHint(QPainter::Antialiasing);
        pp.view->setFrameShape(QFrame::NoFrame);
        pp.view->setStyleSheet("background: transparent;");
        pp.view->setMinimumHeight(260);

        m_paramGrid->addWidget(pp.view, row, col);
        if (++col >= nCols) { col = 0; ++row; }

        m_paramPanels.push_back(pp);
    }
}

// ---------------------------------------------------------------------------
// Parameter charts: refresh data
// ---------------------------------------------------------------------------
void AssimViewer::updateParameterSeries(const QVector<CycleSummary> &cycles)
{
    if (cycles.isEmpty()) return;

    for (ParamPanel &pp : m_paramPanels)
    {
        const int k = pp.paramIndex;

        QVector<QPointF> best, lo, hi;
        best.reserve(cycles.size());
        lo  .reserve(cycles.size());
        hi  .reserve(cycles.size());

        double yMin =  std::numeric_limits<double>::max();
        double yMax = -std::numeric_limits<double>::max();

        // MCMC mode: wipe last refresh's band segments; they are rebuilt
        // below, one QAreaSeries per contiguous run of converged cycles
        // (provisional cycles break the band — transit dispersion is
        // never rendered as posterior width).
        if (m_mcmcMode && m_showParamBand)
        {
            for (QAreaSeries *seg : pp.bandSegments)
            {
                pp.chart->removeSeries(seg);
                seg->deleteLater();
            }
            pp.bandSegments.clear();
        }

        QAbstractAxis *xAx = m_useDateAxis ? (QAbstractAxis*)pp.dateAxis
                                           : (QAbstractAxis*)pp.cycleAxis;
        auto flushBandSegment = [&]()
        {
            // A run of <2 converged cycles has no drawable area; an
            // isolated converged cycle between provisionals therefore
            // shows no band (one point has no visible width).
            if (lo.size() < 2) { lo.clear(); hi.clear(); return; }
            auto *loS = new QLineSeries();
            auto *hiS = new QLineSeries();
            loS->replace(lo);
            hiS->replace(hi);
            auto *area = new QAreaSeries(hiS, loS);
            loS->setParent(area);   // cleaned up with the segment
            hiS->setParent(area);
            QColor bandFill = kBandColor; bandFill.setAlpha(60);
            area->setBrush(QBrush(bandFill));
            area->setPen(QPen(Qt::NoPen));
            pp.chart->addSeries(area);
            area->attachAxis(xAx);
            area->attachAxis(pp.yAxis);
            pp.bandSegments.push_back(area);
            lo.clear(); hi.clear();
        };

        for (const CycleSummary &c : cycles)
        {
            if (k >= c.bestParams.size()) continue;
            const qreal x = cycleX(c, m_useDateAxis);
            const double vBest = c.bestParams.at(k);

            best.push_back(QPointF(x, vBest));
            yMin = std::min(yMin, vBest);
            yMax = std::max(yMax, vBest);

            if (m_showParamBand &&
                k < c.paramP10.size() && k < c.paramP90.size())
            {
                if (m_mcmcMode && !c.converged)
                {
                    // Provisional cycle: point estimate only; the band
                    // breaks here and resumes at the next converged run.
                    flushBandSegment();
                }
                else
                {
                    const double p10 = c.paramP10.at(k);
                    const double p90 = c.paramP90.at(k);
                    lo.push_back(QPointF(x, p10));
                    hi.push_back(QPointF(x, p90));
                    yMin = std::min(yMin, p10);
                    yMax = std::max(yMax, p90);
                }
            }
        }
        pp.bestSeries->replace(best);
        if (m_mcmcMode)
        {
            if (m_showParamBand)
                flushBandSegment();   // close the trailing converged run
        }
        else if (m_showParamBand && pp.p10Series && pp.p90Series)
        {
            pp.p10Series->replace(lo);
            pp.p90Series->replace(hi);
        }

        if (yMax > yMin)
        {
            const double pad = (yMax - yMin) * 0.08 + 1e-9;
            pp.yAxis->setRange(yMin - pad, yMax + pad);
        }
    }
}

// ---------------------------------------------------------------------------
// Diagnostics tab (MCMC mode): four charts of per-cycle sampler health,
// fed from the MCMC-mode extras on CycleSummary (populated by
// PosteriorHistoryLoader; the tab is never built in GA mode).
// ---------------------------------------------------------------------------
void AssimViewer::buildDiagnosticsTab()
{
    auto *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *host = new QWidget();
    auto *grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);

    const QStringList titles = {
        "Effective sample size",
        "Plateaued fraction at deadline",
        "Acceptance rate",
        "Pool size (retained samples)"
    };

    for (int i = 0; i < titles.size(); ++i)
    {
        DiagPanel dp;
        dp.title = titles.at(i);

        dp.chart = new QChart();
        dp.chart->setTitle(dp.title);
        dp.chart->setTitleFont(QFont("Inter", 11, QFont::DemiBold));
        dp.chart->setTitleBrush(QBrush(kTitleColor));
        dp.chart->setBackgroundBrush(QBrush(QColor("#FFFFFF")));
        dp.chart->setBackgroundPen(QPen(QColor("#E1E5EB")));
        dp.chart->setBackgroundRoundness(12);
        dp.chart->setMargins(QMargins(10, 8, 10, 8));
        dp.chart->setPlotAreaBackgroundVisible(false);
        dp.chart->legend()->setVisible(false);

        dp.dateAxis  = new QDateTimeAxis();
        dp.dateAxis->setFormat("yyyy-MM-dd");
        dp.cycleAxis = new QValueAxis();
        dp.cycleAxis->setLabelFormat("%d");
        dp.yAxis     = new QValueAxis();
        for (QAbstractAxis *a : { (QAbstractAxis*)dp.dateAxis,
                                 (QAbstractAxis*)dp.cycleAxis,
                                 (QAbstractAxis*)dp.yAxis })
        {
            a->setLabelsFont(QFont("Inter", 9));
            a->setLabelsColor(kLabelColor);
            a->setGridLineColor(kGridColor);
            a->setLinePenColor(kGridColor);
        }
        dp.chart->addAxis(dp.dateAxis,  Qt::AlignBottom);
        dp.chart->addAxis(dp.cycleAxis, Qt::AlignBottom);
        dp.chart->addAxis(dp.yAxis,     Qt::AlignLeft);
        dp.dateAxis ->setVisible(m_useDateAxis);
        dp.cycleAxis->setVisible(!m_useDateAxis);

        dp.series = new QLineSeries();
        QPen pen{QColor(kLineColors[i % kLineColors.size()])};
        pen.setWidthF(2.4);
        pen.setCapStyle(Qt::RoundCap);
        dp.series->setPen(pen);
        dp.chart->addSeries(dp.series);
        dp.series->attachAxis(m_useDateAxis ? (QAbstractAxis*)dp.dateAxis
                                            : (QAbstractAxis*)dp.cycleAxis);
        dp.series->attachAxis(dp.yAxis);

        dp.view = new QChartView(dp.chart);
        dp.view->setRenderHint(QPainter::Antialiasing);
        dp.view->setFrameShape(QFrame::NoFrame);
        dp.view->setStyleSheet("background: transparent;");
        dp.view->setMinimumHeight(260);

        grid->addWidget(dp.view, i / 2, i % 2);
        m_diagPanels.push_back(dp);
    }

    scroll->setWidget(host);
    m_tabs->addTab(scroll, "Diagnostics");
}

// ---------------------------------------------------------------------------
void AssimViewer::updateDiagnosticsSeries(const QVector<CycleSummary> &cycles)
{
    if (cycles.isEmpty() || m_diagPanels.isEmpty()) return;

    const auto valueFor = [](const CycleSummary &c, int i) -> double
    {
        switch (i)
        {
        case 0:  return c.ess;
        case 1:  return c.plateauedFraction;
        case 2:  return c.acceptanceRate;
        default: return c.poolSize;
        }
    };

    qreal xMin = cycleX(cycles.first(), m_useDateAxis);
    qreal xMax = cycleX(cycles.last(),  m_useDateAxis);
    if (xMax <= xMin)
    {
        // Single record (or degenerate order): pad so the point is
        // centered instead of leaving the axis at its epoch default.
        const qreal pad = m_useDateAxis ? 43200000.0   // ±12 h in ms
                                        : 1.0;         // ±1 cycle
        xMin -= pad;
        xMax += pad;
    }

    for (int i = 0; i < m_diagPanels.size(); ++i)
    {
        DiagPanel &dp = m_diagPanels[i];

        QVector<QPointF> pts;
        pts.reserve(cycles.size());
        double yMin =  std::numeric_limits<double>::max();
        double yMax = -std::numeric_limits<double>::max();

        for (const CycleSummary &c : cycles)
        {
            const double v = valueFor(c, i);
            pts.push_back(QPointF(cycleX(c, m_useDateAxis), v));
            yMin = std::min(yMin, v);
            yMax = std::max(yMax, v);
        }
        dp.series->replace(pts);

        const double pad = (yMax > yMin)
                               ? (yMax - yMin) * 0.08
                               : std::max(0.5, std::abs(yMax) * 0.1);
        dp.yAxis->setRange(yMin - pad, yMax + pad);

        dp.dateAxis->setRange(
            QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(xMin)),
            QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(xMax)));
        dp.cycleAxis->setRange(xMin, xMax);

    }
}

// ---------------------------------------------------------------------------
// X-axis toggle: rewire which axis is attached to each series.
// ---------------------------------------------------------------------------
void AssimViewer::applyXAxisToAllCharts()
{
    auto retarget = [this](QAbstractSeries *s,
                           QAbstractAxis *dateAx,
                           QAbstractAxis *cycleAx)
    {
        if (!s) return;
        // Detach any currently-attached x-axes.
        const auto attached = s->attachedAxes();
        for (auto *a : attached)
        {
            if (a == dateAx || a == cycleAx) s->detachAxis(a);
        }
        QAbstractAxis *want = m_useDateAxis ? dateAx : cycleAx;
        if (want) s->attachAxis(want);
    };

    // Fitness charts (per metric panel).
    for (FitnessLineSpec &f : m_fitnessLines)
    {
        for (int m = 0; m < m_fitnessCharts.size(); ++m)
        {
            if (m >= f.series.size() || !f.series.at(m)) continue;
            const FitnessChartPanel &fp = m_fitnessCharts.at(m);
            retarget(f.series.at(m), fp.dateAxis, fp.cycleAxis);
        }
    }
    for (FitnessChartPanel &fp : m_fitnessCharts)
    {
        if (fp.dateAxis)  fp.dateAxis ->setVisible(m_useDateAxis);
        if (fp.cycleAxis) fp.cycleAxis->setVisible(!m_useDateAxis);
    }

    // Parameter charts.
    for (ParamPanel &pp : m_paramPanels)
    {
        retarget(pp.bestSeries, pp.dateAxis, pp.cycleAxis);
        retarget(pp.bandArea,   pp.dateAxis, pp.cycleAxis);
        for (QAreaSeries *seg : pp.bandSegments)
            retarget(seg, pp.dateAxis, pp.cycleAxis);
        if (pp.dateAxis)  pp.dateAxis ->setVisible(m_useDateAxis);
        if (pp.cycleAxis) pp.cycleAxis->setVisible(!m_useDateAxis);
    }

    // Diagnostics charts (MCMC mode).
    for (DiagPanel &dp : m_diagPanels)
    {
        retarget(dp.series, dp.dateAxis, dp.cycleAxis);
        if (dp.dateAxis)  dp.dateAxis ->setVisible(m_useDateAxis);
        if (dp.cycleAxis) dp.cycleAxis->setVisible(!m_useDateAxis);
    }

    // X ranges based on the data we have.
    if (m_lastCycles.isEmpty()) return;
    qreal xMin = cycleX(m_lastCycles.first(), m_useDateAxis);
    qreal xMax = cycleX(m_lastCycles.last(),  m_useDateAxis);
    if (xMax <= xMin)
    {
        const qreal pad = m_useDateAxis ? 43200000.0 : 1.0;
        xMin -= pad;
        xMax += pad;
    }

    auto setX = [&](QDateTimeAxis *dateAx, QValueAxis *cycleAx)
    {
        if (m_useDateAxis && dateAx)
        {
            dateAx->setRange(
                QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(xMin)),
                QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(xMax)));
        }
        else if (cycleAx)
        {
            const qreal padPos = std::max<qreal>(1.0, (xMax - xMin) * 0.05);
            cycleAx->setRange(xMin - padPos, xMax + padPos);
        }
    };
    for (FitnessChartPanel &fp : m_fitnessCharts)
        setX(fp.dateAxis, fp.cycleAxis);
    for (ParamPanel &pp : m_paramPanels)
        setX(pp.dateAxis, pp.cycleAxis);
}

// ---------------------------------------------------------------------------
// Comparison tab: observed vs modeled time-series overlays
// ---------------------------------------------------------------------------
void AssimViewer::onObservedFailed(const QString &errorMessage)
{
    qWarning() << "[AssimViewer] observed CSV fetch failed:" << errorMessage;
}

void AssimViewer::onModeledFailed(const QString &errorMessage)
{
    qWarning() << "[AssimViewer] modeled CSV fetch failed:" << errorMessage;
}

void AssimViewer::onObservedLoaded(const QVector<CsvSeries> &series)
{
    m_lastObserved = series;
    m_haveObserved = true;
    if (m_haveModeled) updateComparisonPanels();
}

void AssimViewer::onModeledLoaded(const QVector<CsvSeries> &series)
{
    m_lastModeled = series;
    m_haveModeled = true;
    if (m_haveObserved) updateComparisonPanels();
}

// Build a panel per series; key off the union of observed + modeled
// names so a series that exists only on one side still gets a panel
// (the missing side renders empty).
void AssimViewer::rebuildComparisonPanels(const QVector<CsvSeries> &observed,
                                          const QVector<CsvSeries> &modeled)
{
    // Tear down existing.
    for (ComparisonPanel &p : m_compPanels)
    {
        if (p.view)
        {
            m_compLayout->removeWidget(p.view);
            p.view->deleteLater();
        }
    }
    m_compPanels.clear();

    // Build an ordered union of series names, observed first.
    QStringList orderedNames;
    QSet<QString> seen;
    for (const CsvSeries &s : observed)
        if (!seen.contains(s.name)) { orderedNames.push_back(s.name); seen.insert(s.name); }
    for (const CsvSeries &s : modeled)
        if (!seen.contains(s.name)) { orderedNames.push_back(s.name); seen.insert(s.name); }

    for (const QString &name : orderedNames)
    {
        ComparisonPanel p;
        p.name  = name;
        p.chart = new QChart();
        p.chart->setTitle(name);
        p.chart->setTitleFont(QFont("Inter", 12, QFont::DemiBold));
        p.chart->setTitleBrush(QBrush(kTitleColor));
        p.chart->setBackgroundBrush(QBrush(QColor("#FFFFFF")));
        p.chart->setBackgroundPen(QPen(QColor("#E1E5EB")));
        p.chart->setBackgroundRoundness(12);
        p.chart->setMargins(QMargins(14, 10, 14, 10));
        p.chart->setPlotAreaBackgroundVisible(false);
        p.chart->legend()->setVisible(true);
        p.chart->legend()->setAlignment(Qt::AlignBottom);
        p.chart->legend()->setLabelColor(kLabelColor);

        // Modeled: smooth line, darker / saturated color.
        p.modeledSer = new QLineSeries();
        p.modeledSer->setName("Modeled");
        QPen modeledPen{QColor("#2D7FF9")};
        modeledPen.setWidthF(2.0);
        modeledPen.setCapStyle(Qt::RoundCap);
        p.modeledSer->setPen(modeledPen);

        // Observed: scatter dots, contrasting color.
        p.observedSer = new QScatterSeries();
        p.observedSer->setName("Observed");
        p.observedSer->setMarkerShape(QScatterSeries::MarkerShapeCircle);
        p.observedSer->setMarkerSize(5.0);
        p.observedSer->setColor(QColor("#EF4444"));
        p.observedSer->setBorderColor(QColor("#B91C1C"));

        p.chart->addSeries(p.modeledSer);
        p.chart->addSeries(p.observedSer);

        p.xAxis = new QDateTimeAxis();
        p.xAxis->setFormat("yyyy-MM-dd");
        p.xAxis->setTitleText("Time");
        p.xAxis->setTitleFont(QFont("Inter", 10, QFont::DemiBold));
        p.xAxis->setLabelsFont(QFont("Inter", 9));
        p.xAxis->setTitleBrush(QBrush(kLabelColor));
        p.xAxis->setLabelsColor(kLabelColor);
        p.xAxis->setGridLineColor(kGridColor);
        p.xAxis->setLinePenColor(kGridColor);
        p.chart->addAxis(p.xAxis, Qt::AlignBottom);

        p.yAxis = new QValueAxis();
        p.yAxis->setTitleText(name);
        p.yAxis->setTitleFont(QFont("Inter", 10, QFont::DemiBold));
        p.yAxis->setLabelsFont(QFont("Inter", 9));
        p.yAxis->setTitleBrush(QBrush(kLabelColor));
        p.yAxis->setLabelsColor(kLabelColor);
        p.yAxis->setGridLineColor(kGridColor);
        p.yAxis->setLinePenColor(kGridColor);
        p.chart->addAxis(p.yAxis, Qt::AlignLeft);

        p.modeledSer ->attachAxis(p.xAxis);
        p.modeledSer ->attachAxis(p.yAxis);
        p.observedSer->attachAxis(p.xAxis);
        p.observedSer->attachAxis(p.yAxis);

        p.view = new QChartView(p.chart);
        p.view->setRenderHint(QPainter::Antialiasing);
        p.view->setFrameShape(QFrame::NoFrame);
        p.view->setStyleSheet("background: transparent;");
        p.view->setMinimumHeight(280);

        m_compLayout->addWidget(p.view);
        m_compPanels.push_back(p);
    }
}

// Index series by name for fast lookup.
static const CsvSeries *findByName(const QVector<CsvSeries> &all,
                                   const QString &name)
{
    for (const CsvSeries &s : all)
        if (s.name == name) return &s;
    return nullptr;
}

void AssimViewer::updateComparisonPanels()
{
    // Decide whether the panel set needs rebuilding (series names changed).
    QStringList currentNames;
    for (const ComparisonPanel &p : m_compPanels) currentNames.push_back(p.name);

    QStringList newNames;
    QSet<QString> seen;
    for (const CsvSeries &s : m_lastObserved)
        if (!seen.contains(s.name)) { newNames.push_back(s.name); seen.insert(s.name); }
    for (const CsvSeries &s : m_lastModeled)
        if (!seen.contains(s.name)) { newNames.push_back(s.name); seen.insert(s.name); }

    if (newNames != currentNames)
        rebuildComparisonPanels(m_lastObserved, m_lastModeled);

    // Push data into each panel and compute joint axis ranges.
    // For panels whose series is NOT one of the GA's "used" calibration
    // observations (m_lastUsedNames), we hide the observed scatter
    // entirely — those variables don't have meaningful observation
    // data for the assimilation, only the modeled prediction.
    // If we haven't seen a GA result yet (m_lastUsedNames empty),
    // we show observed dots for all panels (conservative default).
    const bool gaSeen = !m_lastUsedNames.isEmpty();

    for (ComparisonPanel &p : m_compPanels)
    {
        const CsvSeries *obs = findByName(m_lastObserved, p.name);
        const CsvSeries *mod = findByName(m_lastModeled,  p.name);

        const bool isCalibrated = !gaSeen || m_lastUsedNames.contains(p.name);

        if (obs && isCalibrated) p.observedSer->replace(obs->points);
        else                     p.observedSer->clear();
        p.observedSer->setVisible(isCalibrated && obs != nullptr);
        // Hide the legend marker for the scatter when we're not drawing it,
        // so the legend doesn't show a stray "Observed" entry.
        const auto markers = p.chart->legend()->markers(p.observedSer);
        for (auto *m : markers) m->setVisible(isCalibrated && obs != nullptr);

        if (mod) p.modeledSer->replace(mod->points);
        else     p.modeledSer->clear();

        // Joint X / Y ranges across whichever sides are present and visible.
        double yMin =  std::numeric_limits<double>::max();
        double yMax = -std::numeric_limits<double>::max();
        qint64 xMin =  std::numeric_limits<qint64>::max();
        qint64 xMax =  std::numeric_limits<qint64>::lowest();

        auto absorb = [&](const CsvSeries *s)
        {
            if (!s || s->points.isEmpty()) return;
            xMin = std::min(xMin, s->xMin);
            xMax = std::max(xMax, s->xMax);
            yMin = std::min(yMin, s->yMin);
            yMax = std::max(yMax, s->yMax);
        };
        if (isCalibrated) absorb(obs);
        absorb(mod);

        if (xMax > xMin)
        {
            p.xAxis->setRange(QDateTime::fromMSecsSinceEpoch(xMin),
                              QDateTime::fromMSecsSinceEpoch(xMax));
        }
        if (yMax > yMin)
        {
            const double pad = (yMax - yMin) * 0.05 + 1e-9;
            p.yAxis->setRange(yMin - pad, yMax + pad);
        }
        else if (yMax == yMin)
        {
            p.yAxis->setRange(yMin - 1.0, yMax + 1.0);
        }
    }
}
