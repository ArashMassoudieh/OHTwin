#pragma once

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <set>
#include <string>
#include <vector>

// Validate the entire update before replacing the last accepted selection.
// Empty lists are rejected here: a partial edit must not enable every series.
inline bool reloadObservationSelection(const QString &path,
                                      const std::set<std::string> &available,
                                      std::vector<std::string> &accepted,
                                      QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = "cannot read observation selection: " + file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    const auto value = doc.object().value("assimilation").toObject()
                          .value("calibration_observations");
    if (parseError.error != QJsonParseError::NoError || !value.isArray()
        || value.toArray().isEmpty()) {
        error = "calibration_observations must be a nonempty array; retaining previous selection";
        return false;
    }
    std::vector<std::string> next;
    std::set<std::string> seen;
    for (const auto &entry : value.toArray()) {
        const auto name = entry.toString().trimmed().toStdString();
        if (!entry.isString() || name.empty() || !available.count(name)
            || !seen.insert(name).second) {
            error = "invalid, duplicate or unknown calibration observation: "
                    + QString::fromStdString(name);
            return false;
        }
        next.push_back(name);
    }
    accepted = std::move(next);
    error.clear();
    return true;
}
