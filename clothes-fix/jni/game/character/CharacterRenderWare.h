#pragma once
#include "game/RW/RenderWare.h"
#include "game/RW/stream.h"
#include <memory>
#include <vector>
struct RwTexDictionary;
namespace Eagle::Character {
// All APIs missing from the existing wrapper are resolved by exported NAME.
// No new base address, version offset, patch or replacement shader is used.
struct RenderWareApi {
    RpClump* (*readClump)(RwStream*)=nullptr;
    RpClump* (*cloneClump)(RpClump*)=nullptr;
    RwBool (*findChunk)(RwStream*,RwUInt32,RwUInt32*,RwUInt32*)=nullptr;
    RpAtomic* (*setHierarchy)(RpAtomic*,RpHAnimHierarchy*)=nullptr;
    RpAtomic* (*setSkinType)(RpAtomic*,RpSkinType)=nullptr;
    RwUInt32 (*skinBones)(RpSkin*)=nullptr;
    const RwMatrix* (*skinMatrices)(RpSkin*)=nullptr;
    char* (*frameName)(RwFrame*)=nullptr;
    RwTexDictionary* (*createDictionary)()=nullptr;
    RwBool (*destroyDictionary)(RwTexDictionary*)=nullptr;
    RwTexDictionary* (*getDictionary)()=nullptr;
    RwTexDictionary* (*setDictionary)(RwTexDictionary*)=nullptr;
    RwTexture* (*addTexture)(RwTexDictionary*,RwTexture*)=nullptr; // returns the texture (0x274120)
    RpMaterial* (*setTexture)(RpMaterial*,RwTexture*)=nullptr;
    bool Resolve();
    static RenderWareApi& Get();
};
struct ClumpDeleter {void operator()(RpClump* p) const {if(p) RpClumpDestroy(p);}};
struct TextureDeleter {void operator()(RwTexture* p) const {if(p&&RwTextureDestroy) RwTextureDestroy(p);}};
using ClumpPtr=std::unique_ptr<RpClump,ClumpDeleter>;
using TexturePtr=std::shared_ptr<RwTexture>;
bool CollectAtomics(RpClump* clump,std::vector<RpAtomic*>& result);
bool FiniteMatrix(const RwMatrix& matrix);
}
