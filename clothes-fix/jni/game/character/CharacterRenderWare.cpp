#include "CharacterRenderWare.h"
#include <dlfcn.h>
#include "CharacterLog.h"
#include <cmath>
namespace Eagle::Character {
RenderWareApi& RenderWareApi::Get() {static RenderWareApi api;return api;}
bool RenderWareApi::Resolve() {
    if(readClump) return true;
    void* lib=dlopen("libGTASA.so",RTLD_NOW|RTLD_NOLOAD);
    if(!lib) return false;
    RenderWareApi next;
#define RW_SYMBOL(field,name) next.field=reinterpret_cast<decltype(next.field)>(dlsym(lib,name)); if(!next.field) {LogLine(ANDROID_LOG_ERROR,"missing RW export: %s",name);dlclose(lib);return false;}
    RW_SYMBOL(readClump,"_Z17RpClumpStreamReadP8RwStream")
    RW_SYMBOL(cloneClump,"_Z12RpClumpCloneP7RpClump")
    RW_SYMBOL(findChunk,"_Z17RwStreamFindChunkP8RwStreamjPjS1_")
    RW_SYMBOL(setHierarchy,"RpSkinAtomicSetHAnimHierarchy")
    RW_SYMBOL(setSkinType,"RpSkinAtomicSetType")
    RW_SYMBOL(skinBones,"RpSkinGetNumBones")
    RW_SYMBOL(skinMatrices,"RpSkinGetSkinToBoneMatrices")
    RW_SYMBOL(frameName,"_Z16GetFrameNodeNameP7RwFrame")
    RW_SYMBOL(createDictionary,"_Z21RwTexDictionaryCreatev")
    RW_SYMBOL(destroyDictionary,"_Z22RwTexDictionaryDestroyP15RwTexDictionary")
    RW_SYMBOL(getDictionary,"_Z25RwTexDictionaryGetCurrentv")
    RW_SYMBOL(setDictionary,"_Z25RwTexDictionarySetCurrentP15RwTexDictionary")
    RW_SYMBOL(addTexture,"_Z25RwTexDictionaryAddTextureP15RwTexDictionaryP9RwTexture")
    RW_SYMBOL(setTexture,"_Z20RpMaterialSetTextureP10RpMaterialP9RwTexture")
#undef RW_SYMBOL
#define RW_OPTIONAL(field,name) next.field=reinterpret_cast<decltype(next.field)>(dlsym(lib,name));
    RW_OPTIONAL(renderPedCB,"_ZN18CVisibilityPlugins11RenderPedCBEP8RpAtomic")
    RW_OPTIONAL(cameraPosn,"_ZN18CVisibilityPlugins14ms_pCameraPosnE")
    RW_OPTIONAL(pedLodDist,"_ZN18CVisibilityPlugins13ms_pedLodDistE")
    RW_OPTIONAL(clumpAlpha,"_ZN18CVisibilityPlugins13GetClumpAlphaEP7RpClump")
    RW_OPTIONAL(enableAlphaModulate,"_Z23emu_EnableAlphaModulatef")
    RW_OPTIONAL(disableAlphaModulate,"_Z24emu_DisableAlphaModulatev")
#undef RW_OPTIONAL
    *this=next;dlclose(lib);return true;
}
bool CollectAtomics(RpClump* clump,std::vector<RpAtomic*>& result) {
    if(!clump) return false;result.clear();result.reserve(32);
    struct Data {std::vector<RpAtomic*>* list;bool overflow=false;} data{&result};
    RpClumpForAllAtomics(clump,[](RpAtomic* atomic,void* pointer)->RpAtomic* {
        auto& d=*static_cast<Data*>(pointer);if(d.list->size()==32) {d.overflow=true;return nullptr;}
        d.list->push_back(atomic);return atomic;
    },&data);
    return !data.overflow&&!result.empty();
}
bool FiniteMatrix(const RwMatrix& m) {
    for(auto v:{m.right,m.up,m.at,m.pos}) if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)) return false;
    return true;
}
}
