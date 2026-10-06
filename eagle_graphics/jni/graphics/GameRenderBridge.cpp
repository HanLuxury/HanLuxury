#include "GameRenderBridge.h"
#include "GraphicsLog.h"
#include "RenderQueueBridge.h"

#include "../main.h"
#include "../util/patch.h"
#include "../game/Scene.h"
#include "../game/Clock.h"
#include "../game/Weather.h"
#include "../game/TimeCycle.h"
#include "../game/game.h"
#include "../game/Entity/Entity.h"
#include "../SkyBox.h"
#include "../game/ObjectSamp.h"

#include <dlfcn.h>
#include <atomic>
#include <cmath>

namespace gfx {
namespace {

constexpr char kTag[] = "Bridge";

using VoidFn = void (*)();

// CLinkList<CPed*> / CLink<CPed*> layout from the libGTASA DWARF (64-bit).
struct GtaLink {
    void* item;     // 0x00
    GtaLink* prev;  // 0x08
    GtaLink* next;  // 0x10
};
struct GtaLinkList {
    GtaLink first;      // 0x00 m_firstLink
    GtaLink last;       // 0x18 m_lastLink
    GtaLink firstFree;  // 0x30 m_firstFreeLink
    GtaLink lastFree;   // 0x48 m_lastFreeLink
    GtaLink* store;     // 0x60 m_pStore
};
static_assert(sizeof(GtaLink) == 0x18 && sizeof(GtaLinkList) == 0x68, "CLinkList layout");
using EntityFn = void (*)(CEntity*);
using QueueFn = void (*)(GtaRenderQueue*);
using MutexFn = void (*)(void*);

struct Symbols {
    void* handle = nullptr;
    GtaRenderQueue** renderQueue = nullptr; // "renderQueue" (RenderQueue*)
    QueueFn flush = nullptr;                // RenderQueue::Flush()
    QueueFn process = nullptr;              // RenderQueue::Process()
    MutexFn mutexObtain = nullptr;          // OS_MutexObtain(void*)
    MutexFn mutexRelease = nullptr;         // OS_MutexRelease(void*)
    void** activeShader = nullptr;          // ES2Shader::activeShader
    EntityFn renderOneNonRoad = nullptr;    // CRenderer::RenderOneNonRoad(CEntity*)
    VoidFn definedState = nullptr;          // DefinedState()
    GtaLinkList* weaponPeds = nullptr;      // CVisibilityPlugins::ms_weaponPedsForPC
    VoidFn renderWeaponPeds = nullptr;      // CVisibilityPlugins::RenderWeaponPedsForPC()
};

Symbols g_sym;
std::atomic<int> g_state{0}; // 0 = not tried, 1 = ok, -1 = failed

template <class T>
bool Resolve(T& out, const char* name, bool required) {
    out = reinterpret_cast<T>(dlsym(g_sym.handle, name));
    if (!out) {
        if (required) GFX_LOGE(kTag, "required symbol missing: %s", name);
        else GFX_LOGW(kTag, "optional symbol missing: %s", name);
    }
    return out != nullptr || !required;
}

Vec3 ToVec(const RwV3d& v) { return {v.x, v.y, v.z}; }

} // namespace

void* GameRenderBridge::Symbol(const char* name) {
    if (!g_sym.handle || !name) return nullptr;
    return dlsym(g_sym.handle, name);
}

bool GameRenderBridge::Init() {
    const int state = g_state.load(std::memory_order_acquire);
    if (state != 0) return state > 0;

    g_sym.handle = CHook::lib ? CHook::lib : dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_sym.handle) {
        GFX_LOGE(kTag, "libGTASA.so handle unavailable");
        g_state.store(-1, std::memory_order_release);
        return false;
    }

    bool ok = true;
    ok &= Resolve(g_sym.renderQueue, "renderQueue", true);
    ok &= Resolve(g_sym.flush, "_ZN11RenderQueue5FlushEv", true);
    ok &= Resolve(g_sym.process, "_ZN11RenderQueue7ProcessEv", true);
    ok &= Resolve(g_sym.mutexObtain, "_Z14OS_MutexObtainPv", true);
    ok &= Resolve(g_sym.mutexRelease, "_Z15OS_MutexReleasePv", true);
    ok &= Resolve(g_sym.activeShader, "_ZN9ES2Shader12activeShaderE", true);
    ok &= Resolve(g_sym.renderOneNonRoad, "_ZN9CRenderer16RenderOneNonRoadEP7CEntity", true);
    ok &= Resolve(g_sym.definedState, "_Z12DefinedStatev", true);
    // Without the list the shadow pass would leak weapon-ped entries: required.
    ok &= Resolve(g_sym.weaponPeds, "_ZN18CVisibilityPlugins18ms_weaponPedsForPCE", true);
    Resolve(g_sym.renderWeaponPeds, "_ZN18CVisibilityPlugins21RenderWeaponPedsForPCEv", false);

    g_state.store(ok ? 1 : -1, std::memory_order_release);
    GFX_LOGI(kTag, "GTA symbol bridge %s", ok ? "ready" : "FAILED (engine disabled)");
    return ok;
}

bool GameRenderBridge::Ready() { return g_state.load(std::memory_order_acquire) > 0; }

GtaRenderQueue* GameRenderBridge::RenderQueue() {
    return (Ready() && g_sym.renderQueue) ? *g_sym.renderQueue : nullptr;
}

void GameRenderBridge::RenderQueueFlush(GtaRenderQueue* q) { if (q && g_sym.flush) g_sym.flush(q); }
void GameRenderBridge::RenderQueueProcess(GtaRenderQueue* q) { if (q && g_sym.process) g_sym.process(q); }
void GameRenderBridge::MutexObtain(void* m) { if (m && g_sym.mutexObtain) g_sym.mutexObtain(m); }
void GameRenderBridge::MutexRelease(void* m) { if (m && g_sym.mutexRelease) g_sym.mutexRelease(m); }
void** GameRenderBridge::ActiveShaderSlot() { return g_sym.activeShader; }

bool GameRenderBridge::CanRenderEntities() {
    return Ready() && g_sym.renderOneNonRoad && g_sym.definedState && RwCameraBeginUpdate;
}

void GameRenderBridge::RenderEntity(CEntity* entity) {
    if (entity && entity->m_pRwObject && g_sym.renderOneNonRoad) g_sym.renderOneNonRoad(entity);
}

void GameRenderBridge::DefinedState() {
    if (g_sym.definedState) g_sym.definedState();
}

void* GameRenderBridge::WeaponPedListMark() {
    return g_sym.weaponPeds ? static_cast<void*>(g_sym.weaponPeds->first.next) : nullptr;
}

int GameRenderBridge::TrimWeaponPedList(void* mark) {
    GtaLinkList* list = g_sym.weaponPeds;
    if (!list || !mark) return 0;
    int removed = 0;
    // Same unlink/relink sequence as the clear loop in Idle() (libGTASA 0x4D9080).
    while (list->first.next != mark && list->first.next != &list->last && removed < 512) {
        GtaLink* link = list->first.next;
        if (!link || !link->next || !link->prev) break;
        link->next->prev = link->prev;
        link->prev->next = link->next;
        link->next = list->firstFree.next;
        list->firstFree.next->prev = link;
        link->prev = &list->firstFree;
        list->firstFree.next = link;
        ++removed;
    }
    return removed;
}

void GameRenderBridge::RenderWeaponPeds() {
    if (g_sym.renderWeaponPeds) g_sym.renderWeaponPeds();
}

bool GameRenderBridge::GetCamera(CameraSnapshot& out) {
    out = CameraSnapshot{};
    RwCamera* cam = Scene.m_pRwCamera;
    if (!cam) return false;
    auto* frame = static_cast<RwFrame*>(rwObjectGetParent(cam));
    if (!frame || !cam->frameBuffer) return false;
    RwMatrix* ltm = RwFrameGetLTM(frame);
    if (!ltm) return false;

    out.camera = cam;
    out.frameBuffer = cam->frameBuffer;
    out.zBuffer = cam->zBuffer;
    out.right = ToVec(ltm->right);
    out.up = ToVec(ltm->up);
    out.at = ToVec(ltm->at);
    out.pos = ToVec(ltm->pos);
    out.viewWindowX = cam->viewWindow.x;
    out.viewWindowY = cam->viewWindow.y;
    out.offsetX = cam->viewOffset.x;
    out.offsetY = cam->viewOffset.y;
    out.nearPlane = cam->nearPlane;
    out.farPlane = cam->farPlane;
    out.projection = static_cast<int>(cam->projectionType);
    out.valid = IsFinite(out.pos) && IsFinite(out.at) && IsFinite(out.right) && IsFinite(out.up) &&
                std::isfinite(out.viewWindowX) && std::isfinite(out.viewWindowY) && out.viewWindowX > 0.0f &&
                out.viewWindowY > 0.0f && out.projection == rwPERSPECTIVE;
    return out.valid;
}

float GameRenderBridge::GetGameHour() {
    // Same time base as CTimeCycle::CalcColoursForPoint (minutes + seconds/60).
    const float minutes = static_cast<float>(CClock::GetGameClockHours()) * 60.0f +
                          static_cast<float>(CClock::GetGameClockMinutes()) +
                          static_cast<float>(CClock::GetGameClockSeconds()) / 60.0f;
    float hour = minutes / 60.0f;
    if (!std::isfinite(hour)) hour = 12.0f;
    return std::fmod(std::max(hour, 0.0f), 24.0f);
}

WeatherSnapshot GameRenderBridge::GetWeather() {
    auto unit = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; };
    WeatherSnapshot w;
    w.rain = unit(CWeather::Rain);
    w.cloud = unit(CWeather::CloudCoverage);
    w.fog = unit(CWeather::Foggyness);
    w.wetRoads = unit(CWeather::WetRoads);
    w.sunGlare = unit(CWeather::SunGlare);
    w.extraSunny = unit(CWeather::ExtraSunnyness);
    w.underwater = unit(CWeather::UnderWaterness);
    w.tunnel = unit(CWeather::InTunnelness);
    w.oldType = CWeather::OldWeatherType;
    w.newType = CWeather::NewWeatherType;
    w.interpolation = unit(CWeather::InterpolationValue);
    return w;
}

Vec3 GameRenderBridge::GetVectorToSun() {
    // m_CurrentStoredValue is a 16-entry ring index (& 0xF in CalcColoursForPoint).
    if (CTimeCycle::m_CurrentStoredValue >= 16) return {0.0f, 0.0f, 1.0f};
    const CVector v = CTimeCycle::GetVectorToSun();
    return Normalized(Vec3{v.x, v.y, v.z}, Vec3{0.0f, 0.0f, 1.0f});
}

bool GameRenderBridge::IsInterior() { return CGame::currArea != 0; }

bool GameRenderBridge::IsSkyObject(const CEntity* entity) {
    CObjectSamp* sky = CSkyBox::GetSkyObject();
    return sky && entity && reinterpret_cast<const CEntity*>(sky->m_pEntity) == entity;
}

} // namespace gfx
