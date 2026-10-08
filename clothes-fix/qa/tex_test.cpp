#include "game/character/CharacterTexture.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>
// ---- fake RenderWare driver ----
enum Mode {Exact,RoundPot,Depth16,LockFails,WideStride};
static Mode g_mode=Exact; static int g_rasters=0,g_textures=0,g_locked=0;
static int Pot(int v){int p=1;while(p<v)p<<=1;return p;}
static RwRaster* FakeCreate(RwInt32 w,RwInt32 h,RwInt32 d,RwInt32){auto*r=(RwRaster*)calloc(1,sizeof(RwRaster));
  if(g_mode==RoundPot){w=Pot(w);h=Pot(h);} r->width=w;r->height=h;r->depth=g_mode==Depth16?16:d;++g_rasters;return r;}
static RwUInt8* FakeLock(RwRaster* r,RwUInt8,RwInt32){if(g_mode==LockFails)return nullptr;++g_locked;
  r->stride=r->width*(r->depth/8)+(g_mode==WideStride?64:0);r->cpPixels=(RwUInt8*)malloc(size_t(r->stride)*r->height);return r->cpPixels;}
static RwRaster* FakeUnlock(RwRaster* r){--g_locked;return r;}
static RwBool FakeDestroy(RwRaster* r){free(r->cpPixels);free(r);--g_rasters;return 1;}
RwRaster* (*RwRasterCreate)(RwInt32,RwInt32,RwInt32,RwInt32)=FakeCreate;
RwUInt8* (*RwRasterLock)(RwRaster*,RwUInt8,RwInt32)=FakeLock;
RwRaster* (*RwRasterUnlock)(RwRaster*)=FakeUnlock;
RwBool (*RwRasterDestroy)(RwRaster*)=FakeDestroy;
RwTexture* RwTextureCreate(RwRaster* raster){auto*t=(RwTexture*)calloc(1,sizeof(RwTexture));t->raster=raster;++g_textures;return t;}
static void FreeTexture(RwTexture* t){FakeDestroy(t->raster);free(t);--g_textures;}
using namespace Eagle::Character;
int main(int argc,char**argv){
  const std::string dir=argv[1]; int ok=0,checked=0;
  DIR* d=opendir(dir.c_str()); std::set<std::string> names; while(auto*e=readdir(d)) if(strstr(e->d_name,".png")) names.insert(e->d_name); closedir(d);
  for(const auto& n:names){
    std::vector<uint8_t> rgba;int w,h;std::string err; ++checked;
    if(!ReadPng(dir+"/"+n,512,rgba,w,h,err)){printf("FAIL %s: %s\n",n.c_str(),err.c_str());continue;}
    assert(rgba.size()==size_t(w)*h*4);
    for(Mode m:{Exact,RoundPot,WideStride}){
      g_mode=m; RwTexture* t=CreateRgbaTexture(rgba.data(),w,h); assert(t);
      RwRaster* r=t->raster; assert(r->depth==32);
      // corners must hold the image corners in every mode (stretch keeps UV 0..1 on the whole image)
      auto px=[&](int x,int y){return r->cpPixels+size_t(y)*r->stride+size_t(x)*4;};
      assert(!memcmp(px(0,0),&rgba[0],4));
      assert(!memcmp(px(r->width-1,r->height-1),&rgba[(size_t(h-1)*w+(w-1))*4],4));
      // every raster is a power of two now; a power-of-two image is copied byte for byte
      assert((r->width&(r->width-1))==0&&(r->height&(r->height-1))==0&&r->width>=w&&r->height>=h);
      if(r->width==w&&r->height==h) for(int y=0;y<h;y+=7) assert(!memcmp(px(0,y),&rgba[size_t(y)*w*4],size_t(w)*4));
      FreeTexture(t);
    }
    ++ok; printf("ok %-32s %dx%d\n",n.c_str(),w,h);
  }
  std::vector<uint8_t> px(16*16*4,7);
  g_mode=Depth16; assert(!CreateRgbaTexture(px.data(),16,16)); 
  g_mode=LockFails; assert(!CreateRgbaTexture(px.data(),16,16));
  g_mode=Exact; assert(!CreateRgbaTexture(nullptr,16,16)); assert(!CreateRgbaTexture(px.data(),0,16)); assert(!CreateRgbaTexture(px.data(),5000,16));
  std::vector<uint8_t> o;int w,h;std::string err;
  assert(!ReadPng(dir+"/does_not_exist.png",512,o,w,h,err)); printf("missing -> %s\n",err.c_str());
  assert(!ReadPng(argv[2],512,o,w,h,err)); printf("non-png -> %s\n",err.c_str());
  assert(!ReadPng(dir+"/user_eg_skin_m.png",256,o,w,h,err)); printf("too big -> %s\n",err.c_str());
  printf("decoded %d/%d, live rasters=%d textures=%d locked=%d\n",ok,checked,g_rasters,g_textures,g_locked);
  return (ok==checked&&!g_rasters&&!g_textures&&!g_locked)?0:1;
}
