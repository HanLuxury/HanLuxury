#pragma once
// EAGLE graphics engine - hooks (ShadowHook, the framework this client already
// uses through CHook; no AML / GlossHook dependency).
//
//   _ZN22CRealTimeShadowManager6UpdateEv   -> shadow pass point (after PreRender,
//                                             before the main camera is cleared)
//   _ZN9ES2Shader5BuildEPKcS1_             -> shadow receiver injection (GL thread)
//   _ZN8CShadows21StoreShadowForVehicle..  \
//   _ZN8CShadows23StoreShadowForPedObject..  > GTA blob/pole/realtime shadows are
//   _ZN8CShadows19StoreRealTimeShadow..      > skipped while real sun shadows are drawn
//   _ZN8CShadows18StoreShadowForPole..     /
// RenderQueue table entries (rqSelectShader, slot 47) are handled by
// RenderQueueBridge; Render2dStuff is already redirected by this client
// (app/app_game.cpp) and calls GraphicsEngine::OnEndWorld().
//
// Every symbol is exported by libGTASA.so 2.10 arm64 (checked with llvm-nm -D).

namespace gfx::GraphicsHooks {

bool Install();

} // namespace gfx::GraphicsHooks
