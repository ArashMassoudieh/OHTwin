// Isolated reduced-model experiment using the same OHQ System and GA as OHTwin.
#include "System.h"
#include "Script.h"
#include "Command.h"
#include "Source.h"
#include "Object.h"
#include "Quan.h"
#include "observation.h"
#include "Parameter.h"
#include "Precipitation.h"
#include "GA.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc,char**argv) {
 QCoreApplication app(argc,argv);
 if(argc!=5){std::cerr<<"Usage: experiment MODEL RESOURCES INPUT_JSON OUTPUT_DIR\n";return 2;}
 try {
 const QString model=argv[1],resources=QString(argv[2])+"/",out=argv[4];
 QFile input(argv[3]);if(!input.open(QIODevice::ReadOnly))throw std::runtime_error("cannot read input");
 const auto d=QJsonDocument::fromJson(input.readAll()).object();
 const double t0=d["start"].toDouble(),t1=d["end"].toDouble(),trainEnd=d["train_end"].toDouble();
 if(!(t0<trainEnd&&trainEnd<t1))throw std::runtime_error("invalid chronological split");
 QDir().mkpath(out);QDir::setCurrent(QFileInfo(model).absolutePath());
 QFile original(model);if(!original.open(QIODevice::ReadOnly))throw std::runtime_error("cannot read model");
 QString text=QString::fromUtf8(original.readAll());
 text.replace("filename=main_components.json","filename="+resources+"main_components.json");
 text.replace("filename=jm_reservoirs.json","filename="+QFileInfo(model).absolutePath()+"/jm_reservoirs.json");
 QFile resolved(out+"/JM.resolved.ohq");if(!resolved.open(QIODevice::WriteOnly))throw std::runtime_error("cannot write resolved model");resolved.write(text.toUtf8());resolved.close();
 const auto training=d["training"].toArray();
 auto system=[&](double end,bool fitting,const std::vector<double>&params,double start,const std::vector<double>&state) {
  auto s=std::make_unique<System>();s->SetDefaultTemplatePath(resources.toStdString());
  Script script;
  for(const auto&line:text.split('\n')){
   const auto clean=line.trimmed();
   if(clean.isEmpty()||clean.startsWith('#'))continue;
   Command command(clean.toStdString());
   if(!command.Syntax())throw std::runtime_error(command.LastError());
   script.Append(command);
  }
  if(!s->CreateFromScript(script,(resources+"settings.json").toStdString())){
   for(const auto&e:script.Errors())std::cerr<<e<<"\n";
   auto* errors=s->GetErrorHandler();
   for(int i=0;i<errors->Count();i++)std::cerr<<(*errors)[i]->description<<"\n";
   throw std::runtime_error("model construction failed");
  }
  auto *gs=s->object("General Settings");
  gs->Variable("simulation_start_time")->SetProperty(std::to_string(start));
  gs->Variable("simulation_end_time")->SetProperty(std::to_string(end));
  s->SetSystemSettings();s->SetSilent(true);
  CPrecipitation rain;
  for(const auto&r:d["rain"].toArray()){auto a=r.toArray();rain.append(a[0].toDouble(),a[1].toDouble(),a[2].toDouble());}
  s->source("Rain")->Variable("timeseries")->SetTimeSeries(rain);
  for(int i=1;i<=3;i++){
   auto*b=s->object("Soil "+std::to_string(i));const double capacity=b->Variable("capacity")->GetVal();
   b->Variable("Storage")->SetProperty(std::to_string((state.empty()?capacity*d["initial_index"].toDouble()/100:state[(i-1)*2+1])));
   if(!state.empty())s->object("Pond "+std::to_string(i))->Variable("Storage")->SetProperty(std::to_string(state[(i-1)*2]));
  }
  if(!params.empty()){
   s->SetParameterValue("JM_infiltration_rate",params[0]);
   s->SetParameterValue("JM_drainage_rate",params[1]);
  }
  s->ApplyParameters();s->CalcAllInitialValues();
  auto errs=s->VerifyAllQuantities();
  if(errs.Count()){for(int i=0;i<errs.Count();i++)std::cerr<<errs[i]->description<<"\n";throw std::runtime_error("model validation failed");}
  if(fitting){
   TimeSeries<double> obs;
   for(const auto&r:training){auto a=r.toArray();if(a[0].toDouble()>=trainEnd)throw std::runtime_error("holdout leakage");
    // OHQ emits an unevaluated boundary observation at t0. Do not fit
    // against that initialization artifact (first nominal step: .001 d).
    if(a[0].toDouble()>t0+.001)obs.append(a[0].toDouble(),a[1].toDouble());
   }
   for(unsigned i=0;i<s->ObservationsCount();i++)if(s->observation(i)->GetName()=="Soil 1 moisture index"){
    s->observation(i)->Variable("observed_data")->SetTimeSeries(obs);
    s->observation(i)->Variable("error_standard_deviation")->SetProperty(std::to_string(d["observation_sigma"].toDouble()));
   }
  }
  return s;
 };
 auto exportOutputs=[&](System&s,const QString&name){
  TimeSeriesSet<double> exported;
  auto&raw=s.GetObservedOutputs();
  for(size_t i=0;i<raw.size();i++){
   TimeSeries<double> series;series.setName(raw[i].name());
   for(int j=0;j<raw[i].size();j++)if(raw[i].getTime(j)>t0+1e-9)
    series.append(raw[i].getTime(j),raw[i].getValue(j));
   exported.push_back(series);
  }
  exported.write((out+"/"+name).toStdString());
 };
 auto baseline=system(t1,false,{},t0,{});
 if(!baseline->Solve())throw std::runtime_error("baseline solve failed");
 exportOutputs(*baseline,"baseline.csv");
 std::vector<double> params;
 if(d.contains("fixed_parameters")){
  auto fixed=d["fixed_parameters"].toArray();
  if(fixed.size()!=2 || fixed[0].toDouble()<=0 || fixed[1].toDouble()<=0)
   throw std::runtime_error("invalid fixed parameters");
  params={fixed[0].toDouble(),fixed[1].toDouble()};
 }else{
 auto fit=system(trainEnd,true,{},t0,{});
 CGA<System> ga(fit.get());ga.SetParameters(fit->object("Optimizer"));
 ga.filenames.pathname=out.toStdString()+"/";ga.filenames.outputfilename="ga_output.txt";
 fit->SetParameterEstimationMode(parameter_estimation_options::inverse_model);
 ga.optimize();
 for(const std::string name:{"JM_infiltration_rate","JM_drainage_rate"}){
  bool found=false;
  for(int i=0;i<ga.Model_out.Parameters().size();i++)if(ga.Model_out.Parameters()[i]->GetName()==name){params.push_back(ga.Model_out.Parameters()[i]->GetValue());found=true;break;}
  if(!found)throw std::runtime_error("missing fitted parameter");
 }
 }
 auto predicted=system(t1,false,params,t0,{});
 if(!predicted->Solve())throw std::runtime_error("calibrated forecast solve failed");
 exportOutputs(*predicted,"assimilated.csv");
 // Optional causal state updates, isolated from fitting and production.
 // Each centered 15-minute observation becomes available at the END of its bin.
 if(d["rolling_state_test"].toBool(false)){
  auto interpolate=[](System&sys,const std::string&name,double time){
   auto&all=sys.GetObservedOutputs();
   for(size_t i=0;i<all.size();i++)if(all[i].name()==name){
    auto&xy=all[i];
    for(int j=1;j<xy.size();j++)if(xy.getTime(j)>=time && xy.getTime(j-1)<=time){
     const double frac=(time-xy.getTime(j-1))/(xy.getTime(j)-xy.getTime(j-1));
     return xy.getValue(j-1)+frac*(xy.getValue(j)-xy.getValue(j-1));
    }
   }
   throw std::runtime_error("rolling forecast does not bracket requested time");
  };
  QFile csv(out+"/rolling_forecasts.csv");
  if(!csv.open(QIODevice::WriteOnly))throw std::runtime_error("cannot write rolling forecasts");
  QTextStream stream(&csv);stream.setRealNumberPrecision(14);
  stream<<"origin,target,horizon_hours,prior_index,updated_index,persistence,prediction,state_increment_m3,observation_available_at,gain\n";
  System* trajectory=predicted.get();std::unique_ptr<System> updated;
  double nextOrigin=trainEnd;
  for(const auto&row:d["validation"].toArray()){
   const auto a=row.toArray();const double available=a[0].toDouble()+.5/96.;
   if(available+1e-8<nextOrigin || available>=t1-.001)continue;
   const double origin=available, observed=a[1].toDouble();
   std::vector<double> state;
   for(int i=1;i<=3;i++){
    const double area=trajectory->object("Pond "+std::to_string(i))->Variable("area")->GetVal();
    const double capacity=trajectory->object("Soil "+std::to_string(i))->Variable("capacity")->GetVal();
    state.push_back(std::max(0.,interpolate(*trajectory,"Pond "+std::to_string(i)+" water depth",origin)*area));
    state.push_back(std::clamp(interpolate(*trajectory,"Soil "+std::to_string(i)+" moisture index",origin)/100.,0.,1.)*capacity);
   }
   const double capacity=trajectory->object("Soil 1")->Variable("capacity")->GetVal();
   const double prior=100*state[1]/capacity;
   const double gain=d["state_update_gain"].toDouble(.5);
   if(!(gain>=0&&gain<=1))throw std::runtime_error("invalid state update gain");
   const double posterior=std::clamp(prior+gain*(observed-prior),0.,100.);
   const double increment=capacity*(posterior-prior)/100.;state[1]=capacity*posterior/100.;
   auto next=system(t1,false,params,origin,state);
   if(!next->Solve())throw std::runtime_error("state-updated solve failed");
   updated=std::move(next);trajectory=updated.get();
   for(double hours:{1.,6.,24.,48.,72.}){
    const double target=origin+hours/24.;if(target>t1-1e-8)continue;
    stream<<origin<<","<<target<<","<<hours<<","<<prior<<","<<posterior<<","<<observed<<","<<interpolate(*trajectory,"Soil 1 moisture index",target)<<","<<increment<<","<<available<<","<<gain<<"\n";
   }
   nextOrigin=origin+6./24.;
  }
 }
 QJsonObject result{{"infiltration_rate_per_day",params[0]},{"drainage_rate_per_day",params[1]},
  {"training_end_serial",trainEnd},{"holdout_used_for_fitting",false},
  {"initial_unevaluated_boundary_sample_omitted",true},
  {"method","OHQ GA parameter calibration; frozen-parameter conditional hindcast; no state nudging"}};
 result.insert("separate_rolling_state_test",d["rolling_state_test"].toBool(false));
 if(d["rolling_state_test"].toBool(false))result.insert("state_update_gain",d["state_update_gain"].toDouble(.5));
 QFile resultFile(out+"/parameters.json");resultFile.open(QIODevice::WriteOnly);resultFile.write(QJsonDocument(result).toJson());
 std::cout<<"EXPERIMENT COMPLETED\n";
 return 0;
 }catch(const std::exception&e){std::cerr<<"EXPERIMENT FAILED: "<<e.what()<<"\n";return 1;}
}
