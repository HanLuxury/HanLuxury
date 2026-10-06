#pragma once
// EAGLE graphics engine - the ONLY place that talks to GTA code/data.
//
// Everything here is either:
//   * an exported libGTASA.so symbol resolved with dlsym (never exit() on a
//     missing symbol, the feature is disabled instead), or
//   * an existing binding of this client (Scene, CClock, CWeather, CTimeCycle,
//     CRenderer visible list, CWorld sectors, RW wrappers in game/RW).
// No hard-coded offsets are introduced by the graphics engine.

#include "Math/Vector3.h"

#include <cstdint>

struct RwCamera;
struct RwRaster;
struct CEntity;

namespace gfx {

struct GtaRenderQueue;

struct CameraSnapshot {
    bool valid = false;
    RwCamera* camera = nullptr;
    RwRaster* frameBuffer = nullptr;
    RwRaster* zBuffer = nullptr;
    Vec3 pos, right, up, at;               // RwFrameGetLTM of the main camera
    float viewWindowX = 0.5f, viewWindowY = 0.5f;
    float offsetX = 0.0f, offsetY = 0.0f;
    float nearPlane = 0.1f, farPlane = 1000.0f;
    int projection = 1;                    // rwPERSPECTIVE
};

struct WeatherSnapshot {
    float rain = 0.0f, cloud = 0.0f, fog = 0.0f, wetRoads = 0.0f;
    float sunGlare = 0.0f, extraSunny = 0.0f;
    float underwater = 0.0f, tunnel = 0.0f;
    int oldType = 0, newType = 0;
    float interpolation = 0.0f;
};

class GameRenderBridge {
public:
    // Resolves every symbol once. Returns false if a required one is missing.
    static bool Init();
    static bool Ready();
    static void* Symbol(const char* mangledName); // dlsym(libGTASA), nullptr if absent

    // ---- RenderQueue (see RenderQueueBridge.h)
    static GtaRenderQueue* RenderQueue();
    static void RenderQueueFlush(GtaRenderQueue* q);
    static void RenderQueueProcess(GtaRenderQueue* q);
    static void MutexObtain(void* mutex);
    static void MutexRelease(void* mutex);
    static void** ActiveShaderSlot(); // &ES2Shader::activeShader

    // ---- Rendering (game thread)
    static bool CanRenderEntities();
    static void RenderEntity(CEntity* entity);  // CRenderer::RenderOneNonRoad
    static void DefinedState();                 // GTA's default 3D render state

    // CPed::Render() appends the ped to CVisibilityPlugins::ms_weaponPedsForPC
    // (CLinkList<CPed*>, inserted after the head link). Idle() empties that list
    // only after RenderWeaponPedsForPC() of the MAIN pass, so a shadow pass must
    // remove its own entries again or weapons are drawn twice / dropped when the
    // list is full. Mark -> render -> TrimWeaponPedList(mark).
    static void* WeaponPedListMark();
    static int TrimWeaponPedList(void* mark);
    static void RenderWeaponPeds();             // CVisibilityPlugins::RenderWeaponPedsForPC()

    // ---- Game state (game thread)
    static bool GetCamera(CameraSnapshot& out);
    static float GetGameHour();                 // 0..24 incl. minutes/seconds
    static WeatherSnapshot GetWeather();
    static Vec3 GetVectorToSun();               // CTimeCycle current vector (normalized)
    static bool IsInterior();                   // CGame::currArea != 0
    static bool IsSkyObject(const CEntity* entity);
};

} // namespace gfx
