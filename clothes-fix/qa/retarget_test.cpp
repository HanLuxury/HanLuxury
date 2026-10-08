#include "game/character/CharacterRetarget.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>
namespace Eagle::Character {
bool FiniteMatrix(const RwMatrix& m) {
    for(auto v:{m.right,m.up,m.at,m.pos}) if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)) return false;
    return true;
}
}
using namespace Eagle::Character;
struct Skel {std::vector<int> ids;std::vector<RwMatrix> inv;};
static bool Read(const char* path,Skel& s){
  std::ifstream f(path);int n;if(!(f>>n)) return false;s.ids.resize(n);for(auto& i:s.ids) f>>i;s.inv.resize(n);
  for(auto& m:s.inv){float v[16];for(float& x:v) f>>x;m={};m.right={v[0],v[1],v[2]};m.up={v[4],v[5],v[6]};m.at={v[8],v[9],v[10]};m.pos={v[12],v[13],v[14]};}
  return bool(f);
}
int main(int argc,char**argv){
  Skel carrier;if(!Read(argv[1],carrier)){puts("carrier read fail");return 1;}
  std::vector<RpHAnimNodeInfo> info(carrier.ids.size());for(size_t i=0;i<info.size();++i){info[i]={carrier.ids[i],int(i),0,nullptr};}
  RpHAnimHierarchy target{};target.numNodes=int(info.size());target.pNodeInfo=info.data();
  int ok=0,bad=0;float worst=0;
  for(int a=2;a<argc;++a){
    Skel src;if(!Read(argv[a],src)){printf("read fail %s\n",argv[a]);++bad;continue;}
    std::vector<RwMatrix> corr;std::vector<int> idx;
    if(!BuildRetarget(src.inv.data(),src.ids,carrier.inv.data(),&target,corr,idx)||corr.size()!=src.ids.size()){printf("RETARGET FAIL %s\n",argv[a]);++bad;continue;}
    for(auto& c:corr){ // rotation part should stay orthonormal
      auto len=[](RwV3d v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);};
      worst=std::max({worst,float(std::abs(len(c.right)-1)),float(std::abs(len(c.up)-1)),float(std::abs(len(c.at)-1))});
    }
    ++ok;
  }
  printf("%s: retarget ok %d fail %d, worst axis length error %.2e\n",argv[1],ok,bad,worst);
  return bad!=0;
}
