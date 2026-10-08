#include "CharacterManager.h"
#include "CharacterPlayer.h"
#include "../clothes/ClothesNetwork.h"
#include "main.h"
#include "game/game.h"
#include "game/Entity/Ped/Ped.h"
#include "game/Camera.h"
#include "net/netgame.h"
#include "java_systems/cef/CEF.h"
#include "json.hpp"
#include <EGL/egl.h>
#include "CharacterLog.h"
#include <chrono>
#include <deque>
#include <mutex>
#include <cmath>
#include <array>
namespace Eagle::Character {
namespace {
using Json=nlohmann::json;
struct State {
    CharacterCatalog catalog;std::unique_ptr<ClothesStreaming> streaming;std::unique_ptr<FaceManager> faces;
    std::map<uint16_t,AppearanceState> confirmed;std::map<uint16_t,std::unique_ptr<CharacterPlayer>> players;
    bool preview=false,shop=false;int lastReady=-1;PlayerAppearance draft;float angle=0,distance=2.8f,height=.4f;
    uint64_t nextInit=0;EGLContext context=EGL_NO_CONTEXT;
};
// Lifetime is explicit: no RenderWare destructors during process static teardown.
State& S() {static State* value=new State;return *value;}
std::mutex queueMutex;std::deque<std::string> queue,outbound;
uint64_t Now() {return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
void Emit(const char* event,const Json& json) {CCEF::GetEvent(event,json.dump());}
CPed* Ped(uint16_t id) {
    auto* p=id==CPlayerPool::GetLocalPlayerID() ? CLocalPlayer::GetPlayerPed():(CPlayerPool::GetSpawnedPlayer(id) ? CPlayerPool::GetSpawnedPlayer(id)->GetPlayerPed():nullptr);
    return p ? p->m_pPed:nullptr;
}
void ClearPlayers() {auto& s=S();for(auto& pair:s.players) pair.second->DetachCurrent(Ped(pair.first));s.players.clear();}
void StopPreview() {auto& s=S();if(s.preview) {s.preview=false;CCamera::SetBehindPlayer();}}
void ProcessEvent(const std::string& text) {
    auto& s=S();auto j=Json::parse(text,nullptr,false);if(j.is_discarded()||!j.is_object()) return;
    const auto action=j.value("action",std::string());
    if(action=="cancel") {StopPreview();return;}
    if(action=="catalog") {CCEF::GetEvent("eagle_character_catalog",s.catalog.UiJson());return;}
    const auto id=CPlayerPool::GetLocalPlayerID();auto found=s.confirmed.find(id);
    if(found==s.confirmed.end()||!found->second.enabled) {
        if(action=="begin") LogLine(ANDROID_LOG_WARN,"creator waits: no appearance from the server yet for player %u",unsigned(id));
        return;
    }
    if(action=="begin") {
        s.preview=true;s.shop=j.value("mode",std::string())=="shop";s.draft=found->second.appearance;s.angle=0;s.distance=2.8f;s.height=.4f;s.lastReady=-1;
        CCEF::GetEvent("eagle_character_catalog",s.catalog.UiJson());
        Emit("eagle_character_local_state",{{"appearance",Json::parse(AppearanceJson(s.draft))},{"revision",found->second.revision}});return;
    }
    if(!s.preview) return;
    if(action=="preview"&&j.contains("appearance")) {
        PlayerAppearance look;
        if(!ParseAppearance(j["appearance"].dump(),look)||!s.catalog.Validate(look)) {Emit("eagle_character_error",{{"message","Pilihan tidak cocok dengan katalog client."}});return;}
        s.draft=look;s.lastReady=-1;return;
    }
    if(action=="rotate") {float n=j.value("delta",0.f);if(std::isfinite(n)) s.angle=std::remainder(s.angle+std::clamp(n,-.5f,.5f),6.2831853f);}
    if(action=="zoom") {float n=j.value("delta",0.f);if(std::isfinite(n)) s.distance=std::clamp(s.distance+n,1.1f,4.f);}
    if(action=="cameraReset") {s.angle=0;s.distance=2.8f;s.height=.4f;}
    if(action=="focus") {
        const auto area=j.value("area",std::string());
        if(area=="face") {s.height=.65f;s.distance=1.1f;}
        else if(area=="shoes") {s.height=-.75f;s.distance=1.4f;}
        else if(area=="body") {s.height=.15f;s.distance=2.8f;}
    }
}
bool PreviewSet(uint16_t id,int field,int n) {
    auto& s=S();if(!s.preview||id!=CPlayerPool::GetLocalPlayerID()) return false;
    auto next=s.draft;auto values=next.Values();values[size_t(field)]=n;next.Assign(values);
    if(!s.catalog.Validate(next)) return false;s.draft=next;return true;
}
}
bool CharacterManager::HandleCefEvent(const std::string& event,const std::string& json) {
    if(event=="ui"&&json.size()<=1024) {
        auto j=Json::parse(json,nullptr,false);
        if(j.is_object()&&j.contains("v")&&j["v"]=="character") {
            std::lock_guard<std::mutex> lock(queueMutex);
            if(outbound.size()<16) outbound.push_back(json);return true;
        }
    }
    if(event!="eagle_character_local") return false;
    if(json.size()>8192) return true;
    std::lock_guard<std::mutex> lock(queueMutex);
    if(queue.size()<64) queue.push_back(json);return true;
}
void CharacterManager::CancelPreview() {HandleCefEvent("eagle_character_local","{\"action\":\"cancel\"}");}
void CharacterManager::Tick() {
    // The existing RakNet build has threading disabled. New creator requests
    // also leave JNI through this owner-thread queue, never directly from UI.
    std::deque<std::string> sends;{std::lock_guard<std::mutex> lock(queueMutex);sends.swap(outbound);}
    if(pNetGame&&pNetGame->GetRakClient()) for(const auto& message:sends) CCEF::SendEvent("ui",message);
    // RenderWare owns/queues all raster operations. CGame::Process can run
    // without an EGL context in MT mode; moving RW/ped state to the EGL thread
    // would race streaming and model destruction.
    auto& s=S();auto current=eglGetCurrentContext();
    const uint64_t now=Now();
    if(current!=EGL_NO_CONTEXT) {
        if(s.context!=EGL_NO_CONTEXT&&s.context!=current) {ClearPlayers();s.faces.reset();s.streaming.reset();}
        s.context=current;
    }
    if(!s.streaming) {
        if(now<s.nextInit) return;s.nextInit=now+10000;
        std::string error;
        std::string root;bool loaded=false;
        if(RenderWareApi::Get().Resolve()) {
            // initSAMP sets g_pszStorage to <directory>/SAMP/, not the asset
            // root. Prefer the launcher root and also support legacy TESTLIT.
            for(const char* candidate:std::array<const char*,4>{g_pszRootStorage,g_pszStorage,"/storage/emulated/0/TESTLIT","/sdcard/TESTLIT"}) {
                if(!candidate||!*candidate) continue;
                root=candidate;while(root.size()>1&&root.back()=='/') root.pop_back();
                if(s.catalog.Load(root,error)) {loaded=true;break;}
            }
        } else error="required RenderWare exports unavailable";
        if(!loaded) {
            LogLine(ANDROID_LOG_ERROR,"init deferred at %s: %s",root.c_str(),error.c_str());
            Emit("eagle_character_error",{{"message","Aset karakter belum siap. Periksa character.json, clothes.json, dan folder karakter di TESTLIT."}});
            return;
        }
        s.faces=std::make_unique<FaceManager>(root);s.streaming=std::make_unique<ClothesStreaming>(root,s.catalog);
        LogLine(ANDROID_LOG_INFO,"catalog ready revision %u at %s",s.catalog.Revision(),root.c_str());
    }
    s.streaming->Tick(now);
    std::deque<std::string> events;{std::lock_guard<std::mutex> lock(queueMutex);events.swap(queue);}
    for(const auto& e:events) {try {ProcessEvent(e);} catch(const std::exception&) {}}
    const auto local=CPlayerPool::GetLocalPlayerID();
    for(const auto& pair:s.confirmed) {
        auto* ped=Ped(pair.first);auto it=s.players.find(pair.first);
        if(!ped||!pair.second.enabled) {if(it!=s.players.end()) {it->second->DetachCurrent(ped);s.players.erase(it);}continue;}
        if(it==s.players.end()) it=s.players.emplace(pair.first,std::make_unique<CharacterPlayer>(pair.first,s.catalog,*s.streaming,*s.faces)).first;
        it->second->Request(s.preview&&pair.first==local ? s.draft:pair.second.appearance);it->second->Tick(ped);
    }
    if(s.preview) {
        auto active=s.players.find(local);const int ready=active!=s.players.end()&&active->second->Ready();
        if(ready!=s.lastReady) {s.lastReady=ready;Emit("eagle_character_preview_status",{{"ready",ready!=0}});}
        auto* ped=Ped(local);
        if(!ped||ped->IsInVehicle()) StopPreview();
        else {const auto& p=ped->GetPosition();const float a=s.angle+ped->GetHeading();
            CCamera::SetPosition(p.x+std::sin(a)*s.distance,p.y+std::cos(a)*s.distance,p.z+s.height+.1f,0,0,0);
            CCamera::LookAtPoint(p.x,p.y,p.z+s.height,2);
        }
    }
}
void CharacterManager::Reset() {
    auto& s=S();StopPreview();ClearPlayers();s.confirmed.clear();s.faces.reset();s.streaming.reset();s.context=EGL_NO_CONTEXT;s.nextInit=0;
    std::lock_guard<std::mutex> lock(queueMutex);queue.clear();outbound.clear();
}
void CharacterManager::BeforePedDestroyed(CPed* ped) {
    if(!ped) return;
    auto& s=S();for(auto it=s.players.begin();it!=s.players.end();) {
        if(it->second->Ped()==ped) {it->second->DetachCurrent(ped);it=s.players.erase(it);}else ++it;
    }
}
void CharacterManager::RemovePlayer(uint16_t id) {auto& s=S();auto p=s.players.find(id);if(p!=s.players.end()) {p->second->DetachCurrent(Ped(id));s.players.erase(p);}s.confirmed.erase(id);}
void CharacterManager::Receive(const uint8_t* bytes,size_t length) {
    AppearanceState state;if(!ClothesNetwork::Decode(bytes,length,state)) return;
    auto& s=S();auto previous=s.confirmed.find(state.playerId);
    if(previous!=s.confirmed.end()&&previous->second.session==state.session&&state.revision<=previous->second.revision) return;
    if(s.streaming&&!s.catalog.Validate(state.appearance)) return;
    s.confirmed[state.playerId]=state;
    if(state.playerId==CPlayerPool::GetLocalPlayerID()) {
        if(s.preview) s.draft=state.appearance;
        Emit("eagle_character_confirmed",{{"appearance",Json::parse(AppearanceJson(state.appearance))},{"revision",state.revision}});
    }
}
bool CharacterManager::SetSkinTone(uint16_t id,int n) {return PreviewSet(id,2,n);}
bool CharacterManager::SetFace(uint16_t id,int n) {return PreviewSet(id,3,n);}
bool CharacterManager::SetHair(uint16_t id,int n) {return PreviewSet(id,4,n);}
bool CharacterManager::SetHairColor(uint16_t id,int n) {return PreviewSet(id,5,n);}
}
