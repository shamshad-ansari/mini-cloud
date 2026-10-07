#include "mini_cloud/scheduler.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>
using namespace mini_cloud;
int main() {
  std::cout << "scenario,requests,trial,policy,placed,success_pct,cpu_sd_pp,memory_sd_pp,mean_schedule_us\n";
  for (int scenario=0;scenario<4;++scenario) for (int trial=0;trial<10;++trial) {
    std::vector<Resources> stream;
    unsigned seed=100+trial;
    const int n=scenario==0?8:48;
    for(int i=0;i<n;++i) {
      seed=1664525u*seed+1013904223u;
      int kind=(seed>>16)%3;
      stream.push_back(scenario==0?Resources{500,128}:kind==0?Resources{800,64}:kind==1?Resources{100,768}:Resources{400,384});
    }
    for(auto policy:{SchedulingPolicy::first_fit,SchedulingPolicy::least_loaded_dominant_resource}) {
      std::vector<Worker> workers;
      for(int i=0;i<4;++i) workers.push_back({"worker-"+std::to_string(i),{{2000,2048},scenario==2?Resources{1000,1024}:Resources{0,0}}});
      int placed=0; double elapsed=0;
      for(auto request:stream) {
        auto start=std::chrono::steady_clock::now();
        auto selection=choose_worker(policy,workers,request);
        auto stop=std::chrono::steady_clock::now();
        elapsed+=std::chrono::duration<double,std::micro>(stop-start).count();
        if(selection) {workers[*selection].resources.reserved=*checked_add(workers[*selection].resources.reserved,request);++placed;}
      }
      double means[2]={},sq[2]={};
      for(auto w:workers){double u[2]={double(w.resources.reserved.cpu_millicores)/2000,double(w.resources.reserved.memory_mib)/2048};for(int k=0;k<2;++k){means[k]+=u[k]/4;sq[k]+=u[k]*u[k]/4;}}
      std::cout<<(scenario==0?"balanced-light":scenario==1?"mixed-empty":scenario==2?"mixed-preloaded":"mixed-repeat")<<','<<n<<','<<trial<<','<<scheduling_policy_name(policy)<<','<<placed<<','<<100.0*placed/n<<','<<100*std::sqrt(std::max(0.0,sq[0]-means[0]*means[0]))<<','<<100*std::sqrt(std::max(0.0,sq[1]-means[1]*means[1]))<<','<<elapsed/n<<'\n';
    }
  }
}
