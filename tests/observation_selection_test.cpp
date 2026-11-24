#include "DTObservationSelection.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cassert>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    const auto path = dir.filePath("config.json");
    const std::set<std::string> available{"depth", "moisture", "flow"};
    std::vector<std::string> active{"depth", "moisture"};
    QString error;
    auto write = [&](const QByteArray &data) {
        QFile f(path); assert(f.open(QIODevice::WriteOnly)); f.write(data);
    };
    write(R"({"assimilation":{"calibration_observations":["depth","moisture","flow"]}})");
    assert(reloadObservationSelection(path, available, active, error));
    assert(active.size() == 3);
    const auto lastGood = active;
    for (const QByteArray bad : {
             QByteArray("{"),
             QByteArray(R"({"assimilation":{"calibration_observations":[]}})"),
             QByteArray(R"({"assimilation":{"calibration_observations":["flow","missing"]}})"),
             QByteArray(R"({"assimilation":{"calibration_observations":["flow","flow"]}})"),
             QByteArray(R"({"assimilation":{"calibration_observations":[42]}})")}) {
        write(bad);
        assert(!reloadObservationSelection(path, available, active, error));
        assert(active == lastGood);
    }
    write(R"({"assimilation":{"calibration_observations":["depth"]}})");
    assert(reloadObservationSelection(path, available, active, error));
    assert(active == std::vector<std::string>{"depth"});
    QFile::remove(path);
    assert(!reloadObservationSelection(path, available, active, error));
    assert(active == std::vector<std::string>{"depth"});
    return 0;
}
