#pragma once
#include "../clothes/ClothesRenderer.h"
#include "FaceManager.h"
class CPed;
namespace Eagle::Character {
class CharacterPlayer {
public:
    CharacterPlayer(uint16_t playerId,const CharacterCatalog& catalog,ClothesStreaming& streaming,FaceManager& faces);
    ~CharacterPlayer();
    void Request(const PlayerAppearance& look);
    void Tick(CPed* ped);
    void Detach(bool nativeAlive=true);
    void DetachCurrent(CPed* livePed);
    CPed* Ped() const {return ped_;}
    bool Ready() const {return hookedClump_&&!parts_.empty()&&!loading_&&requestedValid_&&active_==requested_;}
private:
    struct Original {RpAtomic* atomic;RpAtomicCallBackRender callback;};
    static RpAtomic* RenderHook(RpAtomic* atomic);
    bool Draw();
    void Failed(const char* why);
    uint16_t id_;const CharacterCatalog& catalog_;ClothesStreaming& streaming_;FaceManager& faces_;
    CPed* ped_=nullptr;RpClump* hookedClump_=nullptr;
    std::vector<Original> originals_;
    PlayerAppearance requested_,active_;bool requestedValid_=false,loading_=false,drawOK_=false,drawLogged_=false;
    bool pedCulling_=false; // the ped rendered through RenderPedCB: parts follow its distance cull and fade
    std::vector<std::shared_ptr<AssetTicket>> pending_;
    std::vector<std::unique_ptr<ClothesRenderer>> parts_;
    TexturePtr face_,body_;uint16_t hidden_=0;
    uint64_t retryAfter_=0;
};
}
