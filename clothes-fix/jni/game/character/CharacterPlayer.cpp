#include "CharacterPlayer.h"
#include "SkinToneManager.h"
#include "HairManager.h"
#include "../clothes/ClothesManager.h"
#include "game/Entity/Ped/Ped.h"
#include "ClothesSystem.h"
#include <unordered_map>
#include "CharacterLog.h"
#include <chrono>
namespace Eagle::Character {
namespace {
std::unordered_map<RpAtomic*,CharacterPlayer*> owners;
uint64_t Clock() {return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
}
CharacterPlayer::CharacterPlayer(uint16_t id,const CharacterCatalog& c,ClothesStreaming& s,FaceManager& f):id_(id),catalog_(c),streaming_(s),faces_(f) {}
CharacterPlayer::~CharacterPlayer() {Detach(false);}
void CharacterPlayer::DetachCurrent(CPed* livePed) {
    Detach(livePed&&livePed==ped_&&livePed->m_pRwClump==hookedClump_);
}
void CharacterPlayer::Request(const PlayerAppearance& look) {
    if(Clock()<retryAfter_) return;
    if(!catalog_.Validate(look)||(requestedValid_&&look==requested_)) return;
    requested_=look;requestedValid_=true;loading_=true;pending_.clear();
    for(const auto& key:catalog_.Required(look)) pending_.push_back(streaming_.Acquire(key));
}
void CharacterPlayer::Failed(const char* why) {
    LogLine(ANDROID_LOG_ERROR,"player %u: %s; keeping the previous look, retry in 6 s",unsigned(id_),why);
    loading_=false;pending_.clear();requestedValid_=false;retryAfter_=Clock()+6000;
}
void CharacterPlayer::Detach(bool nativeAlive) {
    for(const auto& old:originals_) {
        owners.erase(old.atomic);
        if(nativeAlive&&old.atomic->renderCallBack==RenderHook) old.atomic->renderCallBack=old.callback;
    }
    originals_.clear();hookedClump_=nullptr;ped_=nullptr;
}
void CharacterPlayer::Tick(CPed* ped) {
    if(ped_!=ped||(ped&&hookedClump_&&ped->m_pRwClump!=hookedClump_)) {
        const bool alive=ped_&&ped_==ped&&ped_->m_pRwClump==hookedClump_;
        Detach(alive);
    }
    if(!ped||!ped->m_pRwClump) return;
    if(ped->m_nModelIndex!=kMaleCarrier&&ped->m_nModelIndex!=kFemaleCarrier) {
        Detach(ped_==ped&&ped->m_pRwClump==hookedClump_);return;
    }
    ped_=ped;
    if(loading_) {
        for(const auto& t:pending_) {
            if(!t||t->state==AssetTicket::State::Failed) {Failed(t ? "an asset failed to load (see the asset line above)":"asset queue full");return;}
            if(t->state!=AssetTicket::State::Ready) return;
        }
        std::vector<std::unique_ptr<ClothesRenderer>> next;
        const auto required=catalog_.Required(requested_);const auto bodyKey=catalog_.BodyKey(requested_);
        if(required.size()!=pending_.size()) {Failed("catalog changed while loading");return;}
        for(size_t index=0;index<pending_.size();++index) {
            Colour tint{};
            for(int slot=1;slot<int(Slot::Count);++slot) {
                const auto* item=catalog_.Item(requested_.Item(Slot(slot)));
                if(item&&item->variants.at(bodyKey)==required[index]) {tint=item->tint;break;}
            }
            auto part=std::make_unique<ClothesRenderer>(pending_[index],tint);
            if(!part->Initialise()) {Failed(("cannot clone/skin asset "+required[index]).c_str());return;}
            next.push_back(std::move(part));
        }
        auto* frame=RpClumpGetFrame(ped->m_pRwClump);if(!frame) return;
        auto* root=RwFrameGetLTM(frame);if(!root) return;
        for(auto& p:next) if(!p->Prepare(ped->m_pRwClump,*root)) {Failed("skeleton does not map onto the carrier ped");return;}
        parts_=std::move(next);active_=requested_;pending_.clear();loading_=false;
        hidden_=ClothesManager::HiddenRegions(catalog_,active_);
        face_=faces_.Get(active_,true);body_=faces_.Get(active_,false);
    }
    if(hookedClump_||parts_.empty()) return;
    CClothesSystem::ClearPed(ped); // Restore legacy callbacks before taking ownership.
    std::vector<RpAtomic*> atomics;if(!CollectAtomics(ped->m_pRwClump,atomics)) return;
    for(auto* atomic:atomics) if(owners.count(atomic)) return;
    // Allocate all bookkeeping before touching callbacks.
    originals_.reserve(atomics.size());owners.reserve(owners.size()+atomics.size());
    // Never record the hook as an original: falling back to it would recurse without end.
    const auto& rw=RenderWareApi::Get();
    for(auto* atomic:atomics) originals_.push_back({atomic,atomic->renderCallBack==RenderHook ? rw.renderPedCB:atomic->renderCallBack});
    pedCulling_=rw.renderPedCB&&originals_.front().callback==rw.renderPedCB;
    hookedClump_=ped->m_pRwClump;drawLogged_=false;
    LogLine(ANDROID_LOG_INFO,"player %u: %zu parts on ped model %d",unsigned(id_),parts_.size(),int(ped->m_nModelIndex));
    for(const auto& old:originals_) {owners[old.atomic]=this;old.atomic->renderCallBack=RenderHook;}
}
bool CharacterPlayer::Draw() {
    if(!ped_||ped_->m_pRwClump!=hookedClump_||parts_.empty()) return false;
    auto* frame=RpClumpGetFrame(hookedClump_);
    if(!frame) return false;auto* root=RwFrameGetLTM(frame);if(!root) return false;
    // The hook replaces RenderPedCB (0x6fb3cc), so do what it does: nothing past ms_pedLodDist, and the
    // clump alpha fades the parts (alpha/255 through emu_EnableAlphaModulate).
    const auto& rw=RenderWareApi::Get();int alpha=255;
    if(pedCulling_&&rw.cameraPosn&&*rw.cameraPosn&&rw.pedLodDist) {
        const RwV3d& camera=**rw.cameraPosn;
        const float dx=root->pos.x-camera.x,dy=root->pos.y-camera.y,dz=root->pos.z-camera.z;
        if(dx*dx+dy*dy+dz*dz>=*rw.pedLodDist) return true; // the native ped is not drawn either
        if(rw.clumpAlpha) alpha=rw.clumpAlpha(hookedClump_);
    }
    for(auto& part:parts_) if(!part->Prepare(hookedClump_,*root)) return false;
    const auto skin=SkinToneManager::Get(catalog_,active_),hair=HairManager::GetColour(catalog_,active_);
    const bool fade=alpha!=255&&rw.enableAlphaModulate&&rw.disableAlphaModulate;
    if(fade) rw.enableAlphaModulate(float(alpha)/255.f);
    for(auto& part:parts_) part->Render(hidden_,skin,hair,face_.get(),body_.get());
    if(fade) rw.disableAlphaModulate();
    return true;
}
RpAtomic* CharacterPlayer::RenderHook(RpAtomic* atomic) {
    auto found=owners.find(atomic);
    if(found==owners.end()) {const auto& rw=RenderWareApi::Get();return rw.renderPedCB ? rw.renderPedCB(atomic):AtomicDefaultRenderCallBack(atomic);}
    auto& owner=*found->second;
    if(atomic==owner.originals_.front().atomic) {
        owner.drawOK_=owner.Draw();
        if(!owner.drawOK_&&!owner.drawLogged_) {owner.drawLogged_=true;LogLine(ANDROID_LOG_ERROR,"player %u: clothes not drawn this frame, showing the native ped",unsigned(owner.id_));}
    }
    if(!owner.drawOK_) for(const auto& old:owner.originals_) if(old.atomic==atomic)
        return old.callback ? old.callback(atomic):AtomicDefaultRenderCallBack(atomic);
    return atomic;
}
}
