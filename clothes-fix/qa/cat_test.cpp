#include "game/character/CharacterCatalog.h"
#include "game/clothes/ClothesNetwork.h"
#include "json.hpp"
#include <cassert>
#include <cstdio>
#include <set>
using namespace Eagle::Character;
int main(int argc,char**argv){
  CharacterCatalog c;std::string err;
  if(!c.Load(argv[1],err)){printf("LOAD FAIL: %s\n",err.c_str());return 1;}
  printf("catalog revision %u items %zu textures %zu\n",c.Revision(),c.Items().size(),c.Textures().size());
  PlayerAppearance d;std::string why;
  printf("native default valid=%d\n",c.Validate(d,&why));
  int combos=0,bad=0;
  for(int g=0;g<2;++g)for(int b=0;b<3;++b){
    PlayerAppearance a;a.gender=g;a.bodyType=b;
    for(int f=1;f<=12;++f){a.face=f;if(!c.Validate(a,&why)){++bad;printf("face %d g%d b%d: %s\n",f,g,b,why.c_str());}}a.face=1;
    for(int h=0;h<=8;++h){a.hair=h;if(!c.Validate(a,&why)){++bad;printf("hair %d: %s\n",h,why.c_str());}}a.hair=1;
    for(auto& [id,item]:c.Items()){
      PlayerAppearance x=a;x.SetItem(item.slot,id);++combos;
      if(!c.Validate(x,&why)){++bad;printf("item %d g%d b%d: %s\n",id,g,b,why.c_str());continue;}
      for(auto& k:c.Required(x)) if(!c.Asset(k)){++bad;printf("missing asset %s\n",k.c_str());}
    }
  }
  printf("item/body combos %d bad %d\n",combos,bad);
  // packet exactly as character_system.inc EC_SendAppearance writes it, after the 5-byte header
  std::vector<uint8_t> p{1,1, 7,0};
  auto w32=[&](uint32_t v){for(int s=0;s<32;s+=8)p.push_back(uint8_t(v>>s));};
  w32(42);w32(3);p.push_back(1);
  int look[22]={1,2,4,5,6,3,1,0,2,105,203,305,404,0,0,0,0,0,0,0,1,2};
  for(int v:look){p.push_back(uint8_t(v));p.push_back(uint8_t(v>>8));}
  AppearanceState s;bool ok=ClothesNetwork::Decode(p.data(),p.size(),s);
  printf("decode ok=%d id=%u session=%u rev=%u top=%d jacket=%d freckles=%d valid=%d\n",ok,s.playerId,s.session,s.revision,s.appearance.top,s.appearance.jacket,s.appearance.freckles,c.Validate(s.appearance,&why));
  auto ui=nlohmann::json::parse(c.UiJson());
  printf("UiJson %zu bytes, faces[female_large]=%zu hair[male_slim]=%zu items=%zu\n",c.UiJson().size(),ui["faces"]["female_large"].size(),ui["hair"]["male_slim"].size(),ui["items"].size());
  return bad||!ok;
}
