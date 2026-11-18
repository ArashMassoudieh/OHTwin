#include <QDateTime>
// Bootstrapper.cpp

#include "Bootstrapper.h"
#include "MainWindow.h"
#include "AssimViewer.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

Bootstrapper::Bootstrapper(QObject *parent)
    : QObject(parent)
{
}

void Bootstrapper::start()
{
#ifdef Q_OS_WASM
    // WASM: fetch config.json relative to the page URL. Same convention as
    // the rest of the viewer's HTTP fetches (CSV, SVG, viz_state.json).
    m_nam = new QNetworkAccessManager(this);
    connect(m_nam, &QNetworkAccessManager::finished,
            this, &Bootstrapper::onConfigReply);
    m_nam->get(QNetworkRequest(QUrl("config.json?_t=" + QString::number(QDateTime::currentMSecsSinceEpoch()))));
#else
    // Desktop: read config.json from the override path if provided,
    // otherwise from next to the binary. qmake's copy_viewer_config rule
    // deposits the default one there at build time.
    const QString path = m_configPathOverride.isEmpty()
        ? QCoreApplication::applicationDirPath() + "/config.json"
        : m_configPathOverride;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        fatal(QStringLiteral("Failed to open %1: %2").arg(path, f.errorString()));
        return;
    }
    handleConfigData(f.readAll(), QUrl::fromLocalFile(path));
#endif
}

void Bootstrapper::onConfigReply(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError)
    {
        fatal(QStringLiteral("Failed to load config.json:\n%1")
                  .arg(reply->errorString()));
        return;
    }
    handleConfigData(reply->readAll(), reply->url());
}

void Bootstrapper::handleConfigData(const QByteArray &data,
                                    const QUrl &configBaseUrl)
{
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
    {
        fatal(QStringLiteral("config.json is not valid JSON:\n%1")
                  .arg(perr.errorString()));
        return;
    }

    const QJsonObject root = doc.object();
    const QString mode =
        root.value("mode").toString("forward").trimmed().toLower();

    // Legacy flat config: no "mode" key AND no "forward" wrapper. Treat
    // the whole object as the forward sub-block.
    const bool legacyFlat = !root.contains("mode") && !root.contains("forward");

    if (mode == "assimilation")
    {
        const QJsonObject sub = root.value("assimilation").toObject();
        if (sub.isEmpty())
        {
            fatal("config.json has mode=\"assimilation\" but no "
                  "\"assimilation\" block.");
            return;
        }
        auto *w = new AssimViewer(root, sub, configBaseUrl);
        w->setAttribute(Qt::WA_DeleteOnClose);
        w->show();
    }
    else if (mode == "forward" || legacyFlat)
    {
        const QJsonObject sub = legacyFlat ? root
                                           : root.value("forward").toObject();
        if (sub.isEmpty())
        {
            fatal("config.json has mode=\"forward\" but no \"forward\" "
                  "block.");
            return;
        }
        auto *w = new MainWindow(root, sub, configBaseUrl);
        w->setAttribute(Qt::WA_DeleteOnClose);
        w->show();
    }
    else
    {
        fatal(QStringLiteral("config.json has unrecognized mode \"%1\". "
                             "Expected \"forward\" or \"assimilation\".")
                  .arg(mode));
    }
}

void Bootstrapper::fatal(const QString &msg)
{
    QMessageBox::critical(nullptr, "DrywellDTViewer", msg);
    qApp->exit(1);
}
