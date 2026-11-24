#pragma once
#include <QFile>
#include <QStringList>
#include <cmath>
#include <map>
#include <vector>

inline double selectedOutputTime(double t)
{
    return std::round(t * 1e5) / 1e5;
}

struct DTSelectedSeries {
    QString name;
    std::map<double, double> points;
};

// CSV emitted by TimeSeriesSet::write uses unquoted alternating t,value
// columns. Empty pairs are padding, not observations at the OHQ epoch.
inline bool readSelectedOutput(const QString &path,
                               std::vector<DTSelectedSeries> &series,
                               QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    const auto header = QString::fromUtf8(file.readLine()).trimmed().split(',');
    if (header.size() < 2 || header.size() % 2) {
        error = "invalid alternating time/value header"; return false;
    }
    std::vector<DTSelectedSeries> parsed;
    for (int i = 1; i < header.size(); i += 2)
        parsed.push_back({header[i].trimmed(), {}});
    int row = 1;
    while (!file.atEnd()) {
        ++row;
        const auto line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty()) continue;
        const auto cells = line.split(',');
        if (cells.size() > header.size()) {
            error = QString("too many columns at row %1").arg(row); return false;
        }
        for (int i = 0; i < header.size(); i += 2) {
            const auto time = cells.value(i).trimmed();
            const auto value = cells.value(i + 1).trimmed();
            if (time.isEmpty() && value.isEmpty()) continue;
            bool timeOk = false, valueOk = false;
            const double t = time.toDouble(&timeOk), v = value.toDouble(&valueOk);
            if (!timeOk || !valueOk || !std::isfinite(t) || !std::isfinite(v)) {
                error = QString("invalid time/value pair at row %1, column %2").arg(row).arg(i + 1);
                return false;
            }
            parsed[i / 2].points[t] = v;
        }
    }
    series = std::move(parsed);
    return true;
}
