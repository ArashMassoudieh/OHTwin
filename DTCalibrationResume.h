#pragma once
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <vector>
#include <string>
#include <algorithm>

inline int completedCalibrationCycles(const QString &directory)
{
    int cycle = 0;
    QFile parameters(QDir(directory).filePath("parameter_history.csv"));
    if (parameters.open(QIODevice::ReadOnly)) {
        while (!parameters.atEnd()) {
            const auto fields = QString::fromUtf8(parameters.readLine()).trimmed().split(',');
            bool ok = false;
            const int n = fields.value(0).toInt(&ok);
            if (ok && n > 0 && fields.size() >= 4) cycle = std::max(cycle, n);
        }
    }
    QFile archive(QDir(directory).filePath("ga_output_merged.txt"));
    if (archive.open(QIODevice::ReadOnly)) {
        const QRegularExpression marker("^=== Cycle ([0-9]+) ", QRegularExpression::MultilineOption);
        auto matches = marker.globalMatch(QString::fromUtf8(archive.readAll()));
        while (matches.hasNext()) cycle = std::max(cycle, matches.next().captured(1).toInt());
    }
    return cycle;
}

inline bool atomicCalibrationCopy(const QString &source, const QString &destination)
{
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly) || in.size() == 0) return false;
    QSaveFile out(destination);
    if (!out.open(QIODevice::WriteOnly)) return false;
    const auto bytes = in.readAll();
    return out.write(bytes) == bytes.size() && out.commit();
}

// ga_output.txt may be a partially-written attempt after a crash. Recover
// population seeds only from the completed archive, never from that live file.
inline QString completedCalibrationPopulation(const QString &directory)
{
    QFile archive(QDir(directory).filePath("ga_output_merged.txt"));
    if (!archive.open(QIODevice::ReadOnly)) return {};
    const auto text = QString::fromUtf8(archive.readAll());
    const QRegularExpression marker("^=== Cycle [0-9]+ [^\\n]*\\n", QRegularExpression::MultilineOption);
    auto matches = marker.globalMatch(text);
    int start = -1;
    while (matches.hasNext()) start = matches.next().capturedEnd();
    if (start < 0) return {};
    const auto population = text.mid(start).toUtf8();
    if (!population.contains("Generation:") || !population.contains("ID,")) return {};
    const auto path = QDir(directory).filePath("ga_population_resume.txt");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(population) != population.size()
        || !file.commit()) return {};
    return path;
}

inline bool calibrationPopulationMatches(const QString &path,
                                         const std::vector<std::string> &parameters)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    while (!file.atEnd()) {
        const auto line = QString::fromUtf8(file.readLine()).trimmed();
        if (!line.startsWith("ID,")) continue;
        const auto columns = line.split(',');
        if (columns.size() < int(parameters.size()) + 2) return false;
        for (size_t i = 0; i < parameters.size(); ++i)
            if (columns[int(i) + 1].trimmed().toStdString() != parameters[i]) return false;
        return true;
    }
    return false;
}
