#pragma once
#include <EGL/egl.h>

// Draw replay uses the game's live GL vertex/fragment programs and buffers.
// No GTA addresses, shader-generator replacement, or CPU mesh/skinning copy.
//
// Every function except Set/GetSettings and SubmitWorldEnd must run on the
// thread that owns the game's EGL context (GTA's GraphicsThread on arm64).
// The game thread uses SubmitWorldEnd(), which queues the work on that thread
// through GraphicsRenderThread in order with the game's own draw commands.
namespace WorldSunShadow {
using HookBackend = void*(*)(void*,void*,void**);
struct Settings {
    bool enabled=true;
    int resolution=1024;   // 512..4096, sun depth map size
    int maxDraws=3000;     // caster budget per frame; extra casters are skipped, never the whole frame
    float radius=70.0f;    // half size of the sun map in metres (shadow distance)
    float darkness=0.50f;
    float softness=1.0f;   // PCF kernel radius in sun-map texels (0.5..3)
};
struct Frame {
    bool valid=false;      // false: no sun shadow this frame; camera/depth capture still run
    float focus[3]{};
    float toSun[3]{0,0,1};
    float sunlight=0;      // Fade to zero at night, in rain/interiors/underwater.
};
// Camera and depth of the world pass that ended last (render thread).
struct WorldView {
    bool valid=false;
    unsigned depthTexture=0;  // depth of the real world target, 0 when it could not be copied
    int width=0,height=0;     // size of depthTexture
    float view[16]{},projection[16]{};
    float clearDepth=1.0f;
};
bool InstallDrawHooks(HookBackend backend);
void SetSettings(const Settings& settings);
Settings GetSettings();
void BeginWorld(const Frame& frame);
void EndWorld(); // The active integration ends before HUD/post-processing.
// Existing Render2dStuff queues sun/weather; EGL swap arms the next world frame.
// The first matching perspective draw obtains the CURRENT camera from GL.
void QueueFrame(const Frame& frame);
void AfterSwap(EGLBoolean result);
// Valid between EndWorld() and the next swap, for the current context only.
bool GetWorldView(WorldView& out);
// Post-processing asks for the depth copy even when sun shadows are off.
void RequestWorldDepth(bool wanted);
void CleanupCurrent(); // Called by the existing EGL lifecycle hooks.
void Retire(EGLDisplay display,EGLContext context);
// Game thread (Render2dStuff): EndWorld() + QueueFrame(next) on the render thread.
void SubmitWorldEnd(const Frame& next);
}
