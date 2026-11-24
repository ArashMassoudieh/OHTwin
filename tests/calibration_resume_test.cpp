#include "DTCalibrationResume.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cassert>
int main(int argc,char **argv) {
 QCoreApplication app(argc,argv);QTemporaryDir dir;
 auto write=[&](const QString &name,const QByteArray &data){QFile f(dir.filePath(name));assert(f.open(QIODevice::WriteOnly));f.write(data);};
 assert(completedCalibrationCycles(dir.path())==0);
 write("parameter_history.csv","cycle,timestamp,t_now,K\n1,2026-01-01,46000,1\n2,2026-01-02,46001,2\ntruncated\n");
 write("ga_output_merged.txt","header\n=== Cycle 1 | timestamp x ===\nGeneration: 0\nID,K,Rank\n0,1,1\n=== Cycle 3 | timestamp y ===\nGeneration: 1\nID,K,Rank\n0,3,1\n");
 write("ga_output.txt","partial failed run: NOT a completed population");
 assert(completedCalibrationCycles(dir.path())==3);
 const auto seed=completedCalibrationPopulation(dir.path());assert(!seed.isEmpty());
 QFile f(seed);assert(f.open(QIODevice::ReadOnly));const auto data=f.readAll();assert(data.contains("0,3,1"));assert(!data.contains("0,1,1"));
 assert(calibrationPopulationMatches(seed,{"K"}));
 assert(!calibrationPopulationMatches(seed,{"changed"}));
 assert(!calibrationPopulationMatches(seed,{"K","extra"}));
 const auto archive=dir.filePath("ga_output_merged.txt");assert(atomicCalibrationCopy(archive,dir.filePath("copy")));
 assert(!atomicCalibrationCopy(dir.filePath("missing"),dir.filePath("copy")));
 QFile unchanged(archive);assert(unchanged.open(QIODevice::ReadOnly));assert(unchanged.readAll().contains("=== Cycle 1"));
 // A damaged archive does not erase valid cycle numbering from parameter history.
 write("ga_output_merged.txt","partial");assert(completedCalibrationCycles(dir.path())==2);
 assert(completedCalibrationPopulation(dir.path()).isEmpty());
 return 0;
}
