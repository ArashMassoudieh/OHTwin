#include "DTSelectedOutput.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cassert>
int main(int argc, char **argv) {
 QCoreApplication app(argc,argv); QTemporaryDir dir;
 const auto path=dir.filePath("output.csv");
 auto write=[&](QByteArray data){QFile f(path);assert(f.open(QIODevice::WriteOnly));f.write(data);};
 assert(selectedOutputTime(46000.000001)==selectedOutputTime(46000.000002));
 assert(selectedOutputTime(46000.000011)>selectedOutputTime(46000.000002));
 std::vector<DTSelectedSeries> result; QString error;
 write("t, Soil moisture,t, Other\n46000,0,46000,1\n46002,2,46001,2\n46001,1,,\n46002,3,,\n");
 assert(readSelectedOutput(path,result,error));
 assert(result.size()==2 && result[0].name=="Soil moisture");
 assert(result[0].points.size()==3 && result[1].points.size()==2);
 assert(result[0].points.at(46000)==0); // real zero reading is preserved
 assert(!result[1].points.count(0)); // padding does not become an epoch sample
 assert(result[0].points.at(46002)==3); // newest duplicate wins
 for(const auto &bad : {"t,a\n46000,\n", "t,a\nwrong,1\n", "t,a\n46000,nan\n", "t,a\n1,2,3\n"}) {
  write(bad); assert(!readSelectedOutput(path,result,error));
  assert(result[0].points.size()==3); // failures leave destination intact
 }
}
