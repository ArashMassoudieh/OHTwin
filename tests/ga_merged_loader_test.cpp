#include <QtTest>
#include <QTemporaryFile>
#include "GaMergedLoader.h"

class GaMergedLoaderTest: public QObject {
    Q_OBJECT
private slots:
    void objectiveHeaderCompatibility_data() {
        QTest::addColumn<QString>("objective");
        QTest::newRow("current-log") << QString("neg_log_likelihood");
        QTest::newRow("historical-log") << QString("likelihood");
    }
    void objectiveHeaderCompatibility() {
        QFETCH(QString,objective);
        QTemporaryFile file;QVERIFY(file.open());
        const QString archive=QString(
            "=== Cycle 1 | timestamp 2026-11-17 23:40:34 | t_now=46343.716330 ===\n"
            "Generation: 0\n"
            "ID, Ksat, %1, Fitness, Rank, Soil 1 moisture_MSE, Soil 1 moisture_R2, Soil 1 moisture_NSE\n"
            "0, 1.2, 71, 1, 1, 0.004, 0.45, -10.1\n"
            "1, 2.4, 90, 0.5, 2, 0.009, 0.2, -20\n").arg(objective);
        file.write(archive.toUtf8());file.flush();
        GaMergedLoader loader;bool loaded=false;QString error;QVector<CycleSummary> parsed;
        connect(&loader,&GaMergedLoader::loaded,this,[&](const QVector<CycleSummary>&v){parsed=v;loaded=true;});
        connect(&loader,&GaMergedLoader::failed,this,[&](const QString&e){error=e;});
        loader.setMergedUrl(QUrl::fromLocalFile(file.fileName()));loader.fetch();
        QTRY_VERIFY_WITH_TIMEOUT(loaded || !error.isEmpty(),2000);
        QVERIFY2(loaded,qPrintable(error));QCOMPARE(parsed.size(),1);
        QCOMPARE(parsed[0].paramNames,QStringList{"Ksat"});
        QCOMPARE(parsed[0].observationNames,QStringList{"Soil 1 moisture"});
        QCOMPARE(parsed[0].bestParams[0],1.2);QCOMPARE(parsed[0].bestNSE[0],-10.1);
        QCOMPARE(parsed[0].paramMin[0],1.2);QCOMPARE(parsed[0].paramMax[0],2.4);
    }
};
QTEST_GUILESS_MAIN(GaMergedLoaderTest)
#include "ga_merged_loader_test.moc"
