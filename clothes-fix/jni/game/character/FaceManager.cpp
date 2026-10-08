#include "FaceManager.h"
#include "CharacterTexture.h"
#include "CharacterLog.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
namespace Eagle::Character {
TexturePtr FaceManager::Get(const PlayerAppearance& a,bool face) {
    if(face ? !(a.eyebrows||a.beard||a.makeup||a.freckles) : !a.tattoo) return {};
    // All face shapes share the donor UV atlas; overlays can share its cache too.
    std::string key=std::to_string(a.gender)+":"+(face ? "f:"+std::to_string(a.eyebrows)+":"+std::to_string(a.beard)+":"+std::to_string(a.makeup)+":"+std::to_string(a.freckles):"b"+std::to_string(a.tattoo));
    if(auto found=cache_[key].lock()) return found;
    for(auto it=cache_.begin();it!=cache_.end();) {if(it->second.expired()) it=cache_.erase(it);else ++it;}
    if(cache_.size()>=96) return {};
    std::string name=face ? "face"+std::to_string(a.face):"skin";
    const std::string donor="user_eg_"+(face ? std::string("face_"):std::string("skin_"))+(a.gender ? "f":"m")+(face ? "_1":"");
    const bool donorAtlas=std::ifstream(root_+"/character/textures/"+donor+".png").good();
    if(donorAtlas) name=donor;
    // Read by full path (see CharacterTexture.h): RtPNGImageRead opened "<storage>/<root>/..." here.
    std::vector<uint8_t> pixels;int w=0,h=0;std::string error;
    if(!ReadPng(root_+"/character/textures/"+name+".png",512,pixels,w,h,error)) {
        LogLine(ANDROID_LOG_ERROR,"overlay %s: %s",name.c_str(),error.c_str());return {};
    }
    // Sample texture UV contract: front is centred on u .25, v .25..80.
    // Multiplicative details keep skin tone identical across the neck seam.
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        // Existing sample atlas is quarter-centred; submitted heads use a
        // centred face layout. Defaults retain the supplied baked face details.
        const float u=(x+.5f)/w-(donorAtlas ? .25f:0.f),v=(y+.5f)/h;float shade=1;
        if(face) {
            if(a.eyebrows&&v>.39f&&v<.39f+.014f*a.eyebrows&&((u>.18f&&u<.24f)||(u>.27f&&u<.33f))) shade=.20f;
            if(a.beard&&v>.62f&&v<.83f&&u>.16f&&u<.34f) shade=a.beard==1 ? .65f:.28f;
            if(a.makeup&&v>.42f&&v<.47f&&((u>.18f&&u<.24f)||(u>.27f&&u<.33f))) shade=.52f;
            if(a.freckles&&v>.47f&&v<.58f&&u>.16f&&u<.34f&&((x*83+y*139)%79)<a.freckles) shade=.48f;
        } else if(a.tattoo&&v>.3f&&v<.55f&&u>.33f&&u<.56f&&((x+y*a.tattoo)%14)<4) shade=.3f;
        for(int c=0;c<3;++c) pixels[(size_t(y)*w+x)*4+c]=uint8_t(pixels[(size_t(y)*w+x)*4+c]*shade);
    }
    // Same size as the atlas: a power-of-two padded raster would shift the UVs of a 104x104 skin.
    TexturePtr texture(CreateRgbaTexture(pixels.data(),w,h),TextureDeleter{});
    if(!texture) return {};
    std::snprintf(texture->name,sizeof(texture->name),"eg_overlay");
    texture->filterAddressing=rwFILTERLINEAR|(rwTEXTUREADDRESSCLAMP<<8)|(rwTEXTUREADDRESSCLAMP<<12);
    cache_[key]=texture;return texture;
}
}
