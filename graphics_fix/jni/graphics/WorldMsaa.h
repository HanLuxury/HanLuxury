#pragma once
// MSAA for the 3D world (GRAFIS "MSAA"), in GTA's own world render target.
//
// libGTASA 2.10: Idle() calls emu_SetAltRenderTarget(w, h) (0x24eb68), which
// creates the world target with RQRenderTarget::Create (0x265d88) -> RQ command
// 26, RQ_Command_rqTargetCreate (0x2663f8): a depth renderbuffer
// (glRenderbufferStorage) and a colour texture (glFramebufferTexture2D). While
// THAT command runs, the two calls are replaced with their
// GL_EXT_multisampled_render_to_texture versions (tile-local MSAA, resolved for
// free when the target is sampled by emu_FlushAltRenderTarget). If the target
// is not complete afterwards, the normal single-sample storage is put back
// and MSAA is switched off for this device. A changed sample count recreates
// the target through GTA's own path (emu_SetAltRenderTarget(0, 0), as during
// a screen fade).
namespace WorldMsaa {
using HookBackend = void* (*)(void* symbol, void* replacement, void** original);

bool InstallHooks(HookBackend backend);
// Render thread, current GTA context.
void CheckDevice();
// Game thread. level: 0 off, 1 = 2x, 2 = 4x.
void SetLevel(int level);
} // namespace WorldMsaa
