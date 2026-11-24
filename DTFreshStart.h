#pragma once
#include <QDir>
#include <QFileInfo>
#include <istream>
#include <string>

inline bool freshContains(const QString &parent, const QString &child)
{
    const QString root = QDir::cleanPath(QFileInfo(parent).absoluteFilePath());
    const QString path = QDir::cleanPath(QFileInfo(child).absoluteFilePath());
    return path == root || path.startsWith(root + '/');
}

inline bool validateFreshTargets(const QString &deployment, QString &error)
{
    const QString root = QFileInfo(deployment).canonicalFilePath();
    if (root.isEmpty() || root == QDir::rootPath()) {
        error = "Refusing reset: invalid deployment directory.";
        return false;
    }
    for (const QString &name : {QString("state"), QString("outputs")}) {
        const QFileInfo target(QDir(root).filePath(name));
        if (target.isSymLink() || (target.exists() &&
            (!target.isDir() || target.canonicalFilePath() != QDir(root).filePath(name)))) {
            error = "Refusing reset of linked or invalid directory: " + target.absoluteFilePath();
            return false;
        }
    }
    return true;
}

inline bool confirmFresh(std::istream &input)
{
    std::string response;
    return bool(std::getline(input, response)) && response == "DELETE";
}
