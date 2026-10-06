#include "GraphicsHooks.h"
#include "GraphicsEngine.h"
#include "GraphicsLog.h"
#include "ShaderPatcher.h"

#include "RQShader.h"                  // project's ES2Shader layout (graphics/RQShader.h)
#include "../modloader/HookScope.h"    // ML_HOOK_SCOPE(): ShadowHook SHARED/UNIQUE aware
#include "shadowhook.h"

#include <cstddef>
#include <exception>

// The receiver registry reads ES2Shader::fullProgram through a raw offset;
// keep it locked to the project's struct (which mirrors the libGTASA DWARF).
static_assert(offsetof(ES2Shader, fullProgram) == gfx::ShaderPatcher::kEs2ShaderProgramOffset,
              "ES2Shader::fullProgram offset changed");

namespace gfx::GraphicsHooks {
namespace {

constexpr char kTag[] = "Hooks";

// ---- CRealTimeShadowManager::Update()  (Idle: after PreRender, before DoRWStuffStartOfFrame)
using UpdateFn = void (*)(void* self);
UpdateFn g_origRealTimeShadowUpdate = nullptr;

void Hook_RealTimeShadowManagerUpdate(void* self) {
    ML_HOOK_SCOPE();
    if (g_origRealTimeShadowUpdate) g_origRealTimeShadowUpdate(self);
    try {
        GraphicsEngine::Get().OnShadowPoint();
    } catch (const std::exception& e) {
        if (GraphicsLog::Once("shadow-point-exception")) GFX_LOGE(kTag, "shadow pass skipped: %s", e.what());
    } catch (...) {
        if (GraphicsLog::Once("shadow-point-exception")) GFX_LOGE(kTag, "shadow pass skipped: unknown exception");
    }
}

// ---- ES2Shader::Build(const char* pixel, const char* vertex)  (RenderQueue thread)
ShaderPatcher::BuildFn g_origBuild = nullptr;

bool Hook_ES2ShaderBuild(void* self, const char* ps, const char* vs) {
    ML_HOOK_SCOPE();
    try {
        return ShaderPatcher::OnBuild(self, ps, vs, g_origBuild);
    } catch (...) {
        if (GraphicsLog::Once("build-exception")) GFX_LOGE(kTag, "shader patch threw, native shader used");
        return g_origBuild ? g_origBuild(self, ps, vs) : false;
    }
}

// ---- GTA projected shadows that real sun shadows replace
bool Suppress() { return GraphicsEngine::Get().ShouldSuppressGtaShadows(); }

using VehicleShadowFn = void (*)(void* vehicle, int type);
VehicleShadowFn g_origVehicleShadow = nullptr;
void Hook_StoreShadowForVehicle(void* vehicle, int type) {
    ML_HOOK_SCOPE();
    if (Suppress()) return;
    if (g_origVehicleShadow) g_origVehicleShadow(vehicle, type);
}

using EntityShadowFn = void (*)(void* entity, float, float, float, float, float, float);
EntityShadowFn g_origPedObjectShadow = nullptr;
void Hook_StoreShadowForPedObject(void* e, float a, float b, float c, float d, float f, float g) {
    ML_HOOK_SCOPE();
    if (Suppress()) return;
    if (g_origPedObjectShadow) g_origPedObjectShadow(e, a, b, c, d, f, g);
}

EntityShadowFn g_origRealTimeShadow = nullptr;
void Hook_StoreRealTimeShadow(void* e, float a, float b, float c, float d, float f, float g) {
    ML_HOOK_SCOPE();
    if (Suppress()) return;
    if (g_origRealTimeShadow) g_origRealTimeShadow(e, a, b, c, d, f, g);
}

using PoleShadowFn = void (*)(void* entity, float, float, float, float, float, unsigned int);
PoleShadowFn g_origPoleShadow = nullptr;
void Hook_StoreShadowForPole(void* e, float a, float b, float c, float d, float f, unsigned int n) {
    ML_HOOK_SCOPE();
    if (Suppress()) return;
    if (g_origPoleShadow) g_origPoleShadow(e, a, b, c, d, f, n);
}

bool HookSymbol(const char* symbol, void* proxy, void** original, bool required) {
    void* stub = shadowhook_hook_sym_name("libGTASA.so", symbol, proxy, original);
    if (!stub || !original || !*original) {
        const int err = shadowhook_get_errno();
        if (required) GFX_LOGE(kTag, "hook FAILED %s: %d %s", symbol, err, shadowhook_to_errmsg(err));
        else GFX_LOGW(kTag, "optional hook failed %s: %d %s", symbol, err, shadowhook_to_errmsg(err));
        return false;
    }
    GFX_LOGI(kTag, "hooked %s", symbol);
    return true;
}

} // namespace

bool Install() {
    static bool installed = false;
    static bool result = false;
    if (installed) return result;
    installed = true;

    // The receiver hook must be in place before GTA builds its first shader.
    const bool build = HookSymbol("_ZN9ES2Shader5BuildEPKcS1_", reinterpret_cast<void*>(&Hook_ES2ShaderBuild),
                                  reinterpret_cast<void**>(&g_origBuild), true);
    const bool update =
        HookSymbol("_ZN22CRealTimeShadowManager6UpdateEv", reinterpret_cast<void*>(&Hook_RealTimeShadowManagerUpdate),
                   reinterpret_cast<void**>(&g_origRealTimeShadowUpdate), true);

    HookSymbol("_ZN8CShadows21StoreShadowForVehicleEP8CVehicle12VEH_SHD_TYPE",
               reinterpret_cast<void*>(&Hook_StoreShadowForVehicle), reinterpret_cast<void**>(&g_origVehicleShadow), false);
    HookSymbol("_ZN8CShadows23StoreShadowForPedObjectEP7CEntityffffff",
               reinterpret_cast<void*>(&Hook_StoreShadowForPedObject), reinterpret_cast<void**>(&g_origPedObjectShadow),
               false);
    HookSymbol("_ZN8CShadows19StoreRealTimeShadowEP9CPhysicalffffff",
               reinterpret_cast<void*>(&Hook_StoreRealTimeShadow), reinterpret_cast<void**>(&g_origRealTimeShadow), false);
    HookSymbol("_ZN8CShadows18StoreShadowForPoleEP7CEntityfffffj", reinterpret_cast<void*>(&Hook_StoreShadowForPole),
               reinterpret_cast<void**>(&g_origPoleShadow), false);

    if (!build) GFX_LOGE(kTag, "no shadow receivers without the ES2Shader::Build hook");
    result = update; // the frame hook is the minimum to run the engine
    return result;
}

} // namespace gfx::GraphicsHooks
