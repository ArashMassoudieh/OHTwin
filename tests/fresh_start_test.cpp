#include "DTFreshStart.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <sstream>
#include <cassert>
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir deployment, outside;
    QString error;
    assert(validateFreshTargets(deployment.path(), error));
    QDir(deployment.path()).mkdir("state");
    QDir(deployment.path()).mkdir("outputs");
    assert(validateFreshTargets(deployment.path(), error));
    assert(QDir(deployment.path()).rmdir("outputs"));
    assert(QFile::link(outside.path(), deployment.filePath("outputs")));
    assert(!validateFreshTargets(deployment.path(), error));
    assert(!validateFreshTargets("/", error));
    assert(freshContains(deployment.path(), deployment.filePath("state")));
    assert(!freshContains(deployment.path(), deployment.path() + "-other"));
    for (const auto &text : {"", "yes\n", "delete\n", "DELETE extra\n"}) {
        std::istringstream input(text);
        assert(!confirmFresh(input));
    }
    std::istringstream yes("DELETE\n");
    assert(confirmFresh(yes));
}
